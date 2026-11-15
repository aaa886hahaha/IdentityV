#ifndef IDV_AUTO_QTE_H
#define IDV_AUTO_QTE_H

// ============================================================================
// 自动完美校准(QTE) —— 照游戏自己的判定算点击时机(2026-11-14 静态，skill idv-reverse-resume/auto-qte.md)
// ============================================================================
//
// 游戏逻辑(ui/UIBattlePart/UIBattleQTE.pyc，UIBattleQTE 是 UIBattleBase 的混入 → 属性挂在 ui_dict['UIBattle'] 实例上)：
//   出题 set_qte_kind / set_group_qte_kind：指针转到 -70°；完美区 exce_pos = (c-4, c+4)，好区 good_pos，全是 int(度)
//   开走 _real_start_qte：inc_flag = True
//   转动 UIBattleBase.tick → tick_qte_ui：inc_flag 为真时每帧 rot += qte_speed × 帧间隔(本地帧时钟，与网络无关)
//   判定 check_qte(qte_btn 的 touch began)：exce_pos[0]-0.1 < rot < exce_pos[1]+0.1 → 完美
//   完美/成功/失败后 inc_flag = False；连续多次(qte_times>1)时 next_qte 直接把 inc_flag 置回 True
//
// 本模块：轮询 inc_flag，看到它从 False 变 True(新的一轮)就记下本机时刻 t0，
//   点击时刻 = t0 + (完美区中心 + 70) / 速度 − 提前量 ± 随机
// 指针角度本身在原生节点里读不到，按时间推；提前量 = 注入触摸 → 游戏下一帧处理 touch began 的本机延迟。
// 完美窗口 8.2° / 80°/s ≈ ±51ms，瞄中心。
//
// 结果标志的坑(反汇编 UIBattleQTE 全部 STORE_ATTR)：qte_perfect_flag / qte_success_flag 只在 init_qte_ui 置 False，
// 之后一局都不复位 → 不能逐轮判完美。能逐轮用的只有 qte_fail_flag(start_qte 复位)。
// 所以日志结果只分「失败 / 命中」，命中里完美与否看画面；本局第一次完美能从 qte_perfect_flag 的跳变认出来。
//
// 不做仲裁(用户 2026-11-14 定)：校准发生在修机/治疗/救人，跟盖板的场景不重叠；
// Touch::InjectTap 内部有互斥锁，同时调用也不会把两段手势交错。
//
// 日志 [校准]：每轮「出题 / 点击 / 结果」三行，用来离线调提前量(探针不可用，靠 pull hack_stdout.log)
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <sched.h>
#include "PyCore.h"
#include "TouchHelperA.h"

namespace AutoQte {

// ---- 界面配置(渲染线程写、校准线程读) ----
static bool  enabled = true;
static float touch_x = 1819.f, touch_y = 956.f;    // 屏幕像素(横屏)，校准按钮。2026-11-14 用户在 3200x1440 上对准后全完美的点击均值
static bool  show_touch_point = false;
static int   lead_ms = 16;          // 提前量：注入到游戏读到按下的延迟。30 时上机落在完美区前 ~30%(偏早约 20ms)，2026-11-14 改 10
static int   time_jitter_ms = 4;    // 点击时刻随机 ±ms(完美窗口 ±51ms，留余量)
static int   hold_ms = 40;
static int   jitter_px = 20;        // 位置随机半径
// 不做按住时长随机：判定只看 touch began，上报只有 qte_status/fail_type/keycode，时长不出客户端

static const float START_ROT = -70.f;
static const int   POLL_US   = 4000;     // 等出题时的轮询间隔(决定 t0 的误差上限)
static const int64_t RESULT_WAIT_MS = 800;

// ---- 面板状态 ----
static char g_status[160] = "";
static char g_last[192] = "-";
static std::atomic<int> g_hit{0}, g_fail{0}, g_skip{0};
static std::atomic<int64_t> g_test_request{0};

static int64_t now_us()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline uint32_t xorshift(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static inline float frand01(uint32_t &s) { return (float)(xorshift(s) >> 8) * (1.0f / 16777216.0f); }
static inline int irand(uint32_t &s, int r) { return r > 0 ? (int)(xorshift(s) % (uint32_t)(2 * r + 1)) - r : 0; }

// ---- 读 UIBattle 实例上的 QTE 字段(校准线程独占，thread_local 序号缓存) ----
struct State {
    bool  inc = false;           // 指针在走
    bool  has_pos = false;
    double exce[2] = {0, 0}, good[2] = {0, 0};   // 普通出题是 int；黑无常(MAGIC_FIELD)/蹦床用 random.uniform 出小数
    float speed = 0;
    int64_t kind = -1, times = -1;
    bool  fail = false, perfect_ever = false;
};

// tuple 元素：int 或 float 都认
static bool read_num_obj(uint64_t vp, double &out)
{
    if (!PyCore::is_obj(vp)) return false;
    uint8_t b[24];
    if (!vm_readv(vp, b, sizeof(b))) return false;
    uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
    if (tp == PyRoot::g_float_type) { memcpy(&out, b + 16, 8); return std::isfinite(out) && fabs(out) < 1000; }
    int64_t v = 0;
    if (!PyCore::read_int(vp, v)) return false;
    out = (double)v;
    return true;
}

static bool read_pair(uint64_t tup, double out[2])
{
    if (!PyCore::is_obj(tup)) return false;
    uint8_t b[0x28];
    if (!vm_readv(tup, b, sizeof(b))) return false;
    int64_t rc = 0, n = 0; memcpy(&rc, b, 8); memcpy(&n, b + 0x10, 8);
    if (rc <= 0 || n != 2) return false;
    uint64_t a = 0, c = 0; memcpy(&a, b + 0x18, 8); memcpy(&c, b + 0x20, 8);
    a &= 0x00FFFFFFFFFFFFFFULL; c &= 0x00FFFFFFFFFFFFFFULL;
    return read_num_obj(a, out[0]) && read_num_obj(c, out[1]) && out[0] <= out[1];
}

// UIBattle 实例 dict：整条链每秒最多重走一次，其余时间直接用缓存的 dict(读失败立即重走)
static uint64_t battle_dict(const char *&why, bool force)
{
    static thread_local uint64_t cached = 0;
    static thread_local int64_t cached_ms = 0;
    static thread_local int64_t h_uimgr = -1, h_uidict = -1, h_battle = -1;
    int64_t t = now_us() / 1000;
    if (!force && cached && t - cached_ms < 1000) return cached;
    cached = 0;
    uint64_t md = PyCore::game_kernel_dict(why);
    if (!md) return 0;
    int64_t iu = PyCore::find_key(md, "g_ui_mgr", h_uimgr);
    if (iu < 0) { why = T("没有 g_ui_mgr", "no g_ui_mgr"); return 0; }
    h_uimgr = iu;
    uint64_t ud = PyCore::get_inst_dict(PyCore::get_attr(md, iu));
    if (!ud) { why = T("g_ui_mgr 实例 dict 无效", "g_ui_mgr dict invalid"); return 0; }
    int64_t id = PyCore::find_key(ud, "ui_dict", h_uidict);
    if (id < 0) { why = T("没有 ui_dict", "no ui_dict"); return 0; }
    h_uidict = id;
    uint64_t uidict = PyCore::get_attr(ud, id);
    int64_t ib = PyCore::find_key(uidict, "UIBattle", h_battle);
    if (ib < 0) { why = T("不在局内", "not in match"); return 0; }
    h_battle = ib;
    uint64_t bd = PyCore::get_inst_dict(PyCore::get_attr(uidict, ib));
    if (!bd) { why = T("UIBattle 实例 dict 无效", "UIBattle dict invalid"); return 0; }
    cached = bd; cached_ms = t;
    return bd;
}

// 按名字取属性值所在的槽(序号缓存 idx)；没有返回 0
static uint64_t slot_of(uint64_t bd, int64_t &idx, const char *name)
{
    PyCore::Dict k;
    if (!PyCore::read_dict(bd, k)) return 0;
    idx = PyCore::resolve_index(bd, k, idx, name);
    if (idx < 0) return 0;
    PyCore::Dict k2;                       // resolve_index 可能全扫过，dict 可能刚扩容：重读一次
    if (!PyCore::read_dict(bd, k2) || idx >= k2.n_entries) return 0;
    return PyCore::value_slot(k2, idx);
}

// light = 只读 inc_flag(等出题时的高频轮询)；否则连出题参数和结果标志一起读
static bool read_state(State &s, bool light, const char *&why)
{
    static thread_local int64_t i_inc = -1, i_exce = -1, i_good = -1, i_speed = -1, i_kind = -1, i_times = -1,
                                i_fail = -1, i_perf = -1;
    s = State();
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t bd = battle_dict(why, attempt > 0);
        if (!bd) return false;
        uint64_t si = slot_of(bd, i_inc, "inc_flag");
        if (!si) { why = T("UIBattle 上没有 inc_flag", "no inc_flag on UIBattle"); continue; }
        if (!PyCore::read_bool(si, s.inc)) { why = "inc_flag"; continue; }
        if (light) { why = ""; return true; }

        uint64_t se = slot_of(bd, i_exce, "exce_pos");
        uint64_t sg = slot_of(bd, i_good, "good_pos");
        uint64_t sp = slot_of(bd, i_speed, "qte_speed");
        s.has_pos = se && sg && read_pair(getPtr64(se), s.exce) && read_pair(getPtr64(sg), s.good);
        if (!sp || !PyCore::read_number(sp, s.speed, 1.0, 2000.0)) s.speed = 0;
        uint64_t f;
        if (!(f = slot_of(bd, i_kind, "qte_kind"))   || !PyCore::read_int(getPtr64(f), s.kind))  s.kind = -1;
        if (!(f = slot_of(bd, i_times, "qte_times")) || !PyCore::read_int(getPtr64(f), s.times)) s.times = -1;
        if ((f = slot_of(bd, i_fail, "qte_fail_flag")))    PyCore::read_bool(f, s.fail);
        if ((f = slot_of(bd, i_perf, "qte_perfect_flag"))) PyCore::read_bool(f, s.perfect_ever);
        why = "";
        return true;
    }
    return false;
}

static int tap(uint32_t &rng, bool exact, float &tx, float &ty, int &hold)
{
    tx = touch_x; ty = touch_y; hold = hold_ms;
    if (!exact && jitter_px > 0) {
        float ang = frand01(rng) * 6.2831853f, rad = frand01(rng) * (float)jitter_px;
        tx += rad * cosf(ang); ty += rad * sinf(ang);
    }
    if (hold < 10) hold = 10;
    return Touch::InjectTap(tx, ty, hold);
}

static inline void request_test_tap() { g_test_request.store(now_us() / 1000); }

static void log_line(const char *s) { printf("[校准] %s\n", s); fflush(stdout); }

static void thread_main()
{
    uint32_t rng = (uint32_t)now_us() | 1;
    bool prev_inc = true;          // 启动时若正在校准，等它走完再说(不知道这一轮的起点)
    char last_why[96] = "";

    while (true) {
        int64_t req = g_test_request.exchange(0);
        if (req != 0 && now_us() / 1000 - req < 1000) {
            float tx, ty; int hold;
            int r = tap(rng, true, tx, ty, hold);
            snprintf(g_last, sizeof(g_last), "test %s (%.0f,%.0f)", Touch::InjectResultText(r), tx, ty);
            log_line(g_last);
        }

        if (!enabled || !PyRoot::g_ready.load()) {
            snprintf(g_status, sizeof(g_status), "%s", enabled ? T("Python 层未就绪", "Python not ready") : T("未开启", "off"));
            prev_inc = true;
            usleep(50000);
            continue;
        }

        State s;
        const char *why = "";
        if (!read_state(s, true, why)) {
            snprintf(g_status, sizeof(g_status), "%s", why);
            if (strcmp(why, last_why) != 0) {           // 原因变了才记一行，免得刷屏
                snprintf(last_why, sizeof(last_why), "%s", why);
                char b[128]; snprintf(b, sizeof(b), "读取失败: %s", why); log_line(b);
            }
            prev_inc = true;
            usleep(100000);
            continue;
        }
        last_why[0] = 0;

        bool rising = s.inc && !prev_inc;
        prev_inc = s.inc;
        if (!rising) {
            snprintf(g_status, sizeof(g_status), "%s", s.inc ? T("校准进行中(起点未知，跳过)", "QTE running (missed start)")
                                                              : T("等待校准", "waiting for QTE"));
            usleep(POLL_US);
            continue;
        }

        // ---- 新的一轮：t0 = 看到指针开走的时刻(误差 ≤ 一个轮询间隔) ----
        int64_t t0 = now_us();
        if (!read_state(s, false, why) || !s.has_pos || s.speed <= 0) {
            g_skip++;
            snprintf(g_last, sizeof(g_last), "跳过: 出题参数读不到(pos=%d speed=%.1f %s)", (int)s.has_pos, s.speed, why);
            log_line(g_last);
            continue;
        }
        bool perfect_before = s.perfect_ever;
        float center = (float)(s.exce[0] + s.exce[1]) * 0.5f;
        float travel_ms = (center - START_ROT) / s.speed * 1000.f;
        int jit = irand(rng, time_jitter_ms);
        int64_t fire = t0 + (int64_t)((travel_ms - (float)lead_ms + (float)jit) * 1000.f);
        {
            char b[192];
            snprintf(b, sizeof(b), "出题 kind=%lld 次数=%lld 完美=[%.1f,%.1f] 好=[%.1f,%.1f] 速度=%.1f 到中心=%.0fms 提前=%d 随机=%+d",
                     (long long)s.kind, (long long)s.times, s.exce[0], s.exce[1],
                     s.good[0], s.good[1], s.speed, travel_ms, lead_ms, jit);
            log_line(b);
        }

        // ---- 等到点击时刻；期间指针停了(被打断/玩家自己点了)就放弃 ----
        bool aborted = false;
        while (true) {
            int64_t left = fire - now_us();
            if (left <= 2000) break;
            usleep(left - 2000 < 4000 ? (useconds_t)(left - 2000) : 4000);
            State c;
            if (!read_state(c, true, why) || !c.inc) { aborted = true; break; }
        }
        if (aborted) {
            g_skip++;
            prev_inc = false;
            snprintf(g_last, sizeof(g_last), "放弃: 点击前这一轮已结束(被打断或已手动点)");
            log_line(g_last);
            continue;
        }
        while (now_us() < fire) sched_yield();       // 最后 2ms 自旋，压掉 usleep 的唤醒误差

        int64_t t_tap = now_us();
        float tx, ty; int hold;
        int r = tap(rng, false, tx, ty, hold);
        {
            char b[192];
            snprintf(b, sizeof(b), "点击 %s (%.0f,%.0f) 按住%dms 偏差=%+.1fms(相对 t0 预定 %.1fms)",
                     Touch::InjectResultText(r), tx, ty, hold, (t_tap - fire) / 1000.0, (fire - t0) / 1000.0);
            log_line(b);
        }
        if (r != Touch::INJECT_OK) {
            g_skip++;
            snprintf(g_last, sizeof(g_last), "注入失败 %s", Touch::InjectResultText(r));
            continue;
        }

        // ---- 等这一轮结束(inc_flag 变 False)，再看失败标志 ----
        const char *res = "未知(800ms 内没结束)";
        int64_t deadline = now_us() + RESULT_WAIT_MS * 1000;
        while (now_us() < deadline) {
            State c;
            if (read_state(c, false, why) && !c.inc) {
                if (c.fail)                                  { res = "失败"; g_fail++; }
                else if (c.perfect_ever && !perfect_before)  { res = "完美(本局首次完美)"; g_hit++; }
                else                                         { res = "命中(完美或成功，看画面)"; g_hit++; }
                break;
            }
            usleep(5000);
        }
        prev_inc = false;
        snprintf(g_last, sizeof(g_last), "%s 到中心%.0fms 提前%d 随机%+d (%.0f,%.0f)", res, travel_ms, lead_ms, jit, tx, ty);
        char b[192]; snprintf(b, sizeof(b), "结果 %s", res); log_line(b);
        snprintf(g_status, sizeof(g_status), "%s", T("已点击", "tapped"));
    }
}

static void start()
{
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(thread_main).detach();
}

static inline const char *status_text() { return g_status[0] ? g_status : T("未开启", "off"); }
static inline const char *last_result() { return g_last; }

} // namespace AutoQte

#endif
