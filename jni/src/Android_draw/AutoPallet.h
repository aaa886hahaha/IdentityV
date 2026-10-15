#ifndef IDV_AUTO_PALLET_H
#define IDV_AUTO_PALLET_H

// ============================================================================
// 自动盖板(砸板) —— 照搬若辰 10.1.74 的判定和门控(2026-09-26 反汇编，skill auto-pallet.md 若辰一节)
// ============================================================================
//
// 若辰原函数(地址是那个二进制里的)：
//   主循环        0x342B00~0x34453C   (Hex-Rays 超时，全部手读汇编)
//   读板子        sub_344D7C          位置 / 状态 / 朝向
//   命中判定      sub_34517C          见下
//   读监管动作号  sub_344A90          小丑拉锯判定用
//   发点击        sub_34453C -> 触摸队列(source "lianxuan_auto_banzi")
//
// 一、输入(全是场景对象，坐标 objcoor=[obj+0x28]，+0xa0/+0xa4/+0xa8 = X/高度/Y)
//   板子   类名含 woodplane 的场景对象(读线程收集后发布过来)
//   状态   u32 [[obj+0x750]+0x30]，板子立着 == 16。读两次 +0x750 指针，一致才算数
//          (帝五忍鸽用的是 +0x730，是旧版本的偏移；若辰的候选表里 0x730 排第二)
//   朝向   objcoor+0xb8 / +0xc0 当作水平方向向量(x, y)，长度 > 1e-5 才可用
//   自身   PySelf::anchor()，必须是求生者(camp == 2)
//   监管   PySelf::hunter_body()；动作号同样是 [[obj+0x750]+0x30]
//
// 二、命中判定 sub_34517C(自身 S, 监管 H, 板子 P，全部只看水平面)
//   state == 16
//   |S-H| / 11.886 <= 3 米
//   |S-P| <= 25 单位(约 2.1 米)                       —— 自己要站在板子边上才放得下
//   普通：把 H-P 投到板子朝向 d 上
//         |along| <= 8.25 + 0.38  且  |perp| <= 半宽 + 0.38
//   小丑拉锯(类名含 butcher_sxwd 且动作号在 0x1802CE±3)：改成 |H-P| <= 80 单位(约 6.7 米)，不需要朝向
//   多块同时命中时取离自己最近的那块
//   若辰在监管是孽蜥时整套不触发(按翻译名"孽蜥"判)，我们去掉了，见 thread_main 里的 TODO
//
// 三、半宽(界面「模式」)
//   暴力 = 12/2、演戏 = 8/2、随机 = 每次点完在 8.0~12.0 之间(步长 0.1)重抽后/2
//
// 四、门控
//   同一块板点过一次就不再点，直到连续 150ms 没有任何候选(监管离开了判定区)
//   两次点击至少间隔 800ms(失败也算一次)
//   点完记下这块板，等它的状态从 16 变成别的值 = 确认放下(只用于统计/面板显示)
//
// 五、跟若辰的不同(有意的)
//   - 若辰还会合并 Python 侧的板子列表(sub_38F028，3 秒内有效)；这里只用场景对象
//   - 若辰对全部监管取最近的一个；这里只看 PySelf 的监管本体(2v8 只看第一个监管)
//   - 若辰在校准(QTE)进行中不点；我们没有自动校准，不需要
//   - 板子位置/朝向是静态的，按对象缓存 2 秒，每轮只重读附近板子的状态
//
// ⚠ 未上机验证(见 逆向存档\_scripts\pallet_probe.py)：
//   状态 16 的含义、+0xb8/+0xc0 是不是板子朝向、小丑拉锯动作号在这一版是不是 0x1802CE
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <unordered_map>
#include <string>
#include "PySelf.h"
#include "TouchHelperA.h"

extern float dist_scale;   // draw_Gui.cpp：1 米 = 11.886 游戏单位

namespace AutoPallet {

// ---- 界面配置(渲染线程写、盖板线程读；跟 show_draw_* 一样直接共享，单个 bool/int/float 的撕裂无害) ----
static bool  enabled = false;
static int   mode = 0;                     // 0 暴力 / 1 演戏 / 2 随机
static float touch_x = 2418.f, touch_y = 1056.f;   // 屏幕像素(横屏方向)，就是游戏里交互(放板)按钮的位置。用户在 3200x1440 上对准的值
static bool  show_touch_point = false;
static bool  show_range = false;           // 把附近板子的判定矩形画在地上
static int   hold_ms = 30;                 // 按住时长
static int   jitter_px = 50;              // 自动点击的位置随机半径(像素)，0 = 不随机。测试点击不随机
static int   jitter_ms = 5;                // 自动点击的按住时长随机 ±ms

// ---- 若辰写死的判定参数(游戏单位) ----
static const float HALF_LEN          = 8.25f;
static const float MARGIN            = 0.38f;
static const float SELF_BOARD_MAX    = 25.0f;
static const float HUNTER_SELF_MAX_M = 3.0f;
static const float CLOWN_RADIUS      = 80.0f;
static const int   CLOWN_ACTION      = 0x1802CE;
static const int   BOARD_STANDING    = 16;
static const int64_t MIN_TAP_INTERVAL_MS = 800;
static const int64_t CLEAR_LAST_BOARD_MS = 150;
static const int   LOOP_US           = 10000;   // 10ms 一轮
static const int   MAX_BOARDS        = 64;
static const int   MAX_RECTS         = 8;

static inline float mode_half_width(int m)
{
    return m == 1 ? 4.0f : 6.0f;           // 演戏 8/2，其余 12/2；随机模式的初值也按 6
}

// ---- 读线程发布过来的板子对象(双缓冲) ----
static uint64_t g_boards[2][MAX_BOARDS];
static int      g_board_n[2] = {0, 0};
static std::atomic<int> g_board_active{0};

static void publish_boards(const uint64_t *b, int n)
{
    int w = 1 - g_board_active.load();
    if (n > MAX_BOARDS) n = MAX_BOARDS;
    memcpy(g_boards[w], b, sizeof(uint64_t) * n);
    g_board_n[w] = n;
    g_board_active.store(w);
}

// ---- 给绘制侧的判定矩形(双缓冲) ----
struct RangeRect {
    uint64_t obj;
    float cx, cy, cz;      // 板子位置 X / Y / 高度
    float ux, uy;          // 单位朝向
    float a, b;            // 沿朝向半长 / 垂直半宽(已含 0.38)
    float r;               // >0 = 小丑拉锯的圆形判定半径
    bool  candidate;       // 这一轮命中的那块
};
static RangeRect g_rects[2][MAX_RECTS];
static int       g_rect_n[2] = {0, 0};
static std::atomic<int> g_rect_active{0};

// ---- 面板状态 ----
static char g_status[160] = "未开启";
static std::atomic<int> g_tap_ok{0}, g_tap_fail{0}, g_confirmed{0};
static char g_last_result[96] = "-";
static float g_half_wid = 6.0f;            // 本轮半宽
static std::atomic<int64_t> g_test_request{0};

static int64_t now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline bool finite3(const float *v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2])
        && fabsf(v[0]) + fabsf(v[1]) + fabsf(v[2]) > 0.0001f;
}

// 场景对象坐标：out = {X, Y, 高度}(跟若辰一样把水平两轴放前面)
static bool read_pos(uint64_t obj, float out[3], uint64_t *coor_out = nullptr)
{
    uint64_t coor = getPtr64(obj + 0x28);
    if (coor < 0x5000000000ULL || coor >= 0x8000000000ULL) return false;
    float raw[3];
    if (!vm_readv(coor + 0xa0, raw, sizeof(raw))) return false;
    out[0] = raw[0]; out[1] = raw[2]; out[2] = raw[1];
    if (coor_out) *coor_out = coor;
    return finite3(out);
}

// [[obj+0x750]+0x30]，前后两次读 +0x750 一致才算数(sub_344D7C 同款)
static bool read_state(uint64_t obj, int &state)
{
    uint64_t c1 = getPtr64(obj + 0x750);
    if (c1 < 0x5000000000ULL || c1 >= 0x8000000000ULL) return false;
    int32_t v = 0;
    if (!vm_readv(c1 + 0x30, &v, 4)) return false;
    if (getPtr64(obj + 0x750) != c1) return false;
    state = v;
    return true;
}

static inline float hdist(const float *a, const float *b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return sqrtf(dx * dx + dy * dy);
}

// 场景对象类名(五跳链，跟读线程同一条)。监管对象一局只有一两个，按地址缓存
static std::string class_name_of(uint64_t obj)
{
    static std::unordered_map<uint64_t, std::pair<uint64_t, std::string>> cache;
    uint64_t head = getPtr64(obj + 0xf8);
    auto it = cache.find(obj);
    if (it != cache.end() && it->second.first == head) return it->second.second;
    uint64_t name_obj = getPtr64(getPtr64(getPtr64(getPtr64(head) + 0x8) + 0x20) + 0x20);
    int len = getDword(name_obj + 0x10);
    if (len <= 0 || len >= 256) return std::string();
    std::string s(len, '\0');
    if (!vm_readv(getPtr64(name_obj + 0x8), &s[0], len)) return std::string();
    if (cache.size() > 16) cache.clear();
    cache[obj] = std::make_pair(head, s);
    return s;
}

// 板子的静态信息(位置/朝向)按对象缓存
struct BoardInfo {
    float pos[3];
    float dir[2];          // 已归一化
    bool  dir_ok;
    int64_t read_ms;
};

// 命中判定，sub_34517C 的等价实现。S=自身 H=监管 P=板子
static bool hit_test(const float *S, const float *H, const BoardInfo &b, float half_wid, bool circle)
{
    if (hdist(S, H) / dist_scale > HUNTER_SELF_MAX_M) return false;
    if (hdist(S, b.pos) > SELF_BOARD_MAX) return false;
    if (circle) return hdist(H, b.pos) <= CLOWN_RADIUS;
    if (!b.dir_ok) return false;
    float dx = H[0] - b.pos[0], dy = H[1] - b.pos[1];
    float along = fabsf(dx * b.dir[0] + dy * b.dir[1]);
    float perp  = fabsf(b.dir[0] * dy - b.dir[1] * dx);
    if (along > HALF_LEN + MARGIN) return false;
    return perp <= half_wid + MARGIN;
}

static inline uint32_t xorshift(uint32_t &s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static inline float frand01(uint32_t &s)   // [0, 1)
{
    return (float)(xorshift(s) >> 8) * (1.0f / 16777216.0f);
}

static void do_tap(const char *source)
{
    int r = Touch::InjectTap(touch_x, touch_y, hold_ms);
    if (r == Touch::INJECT_OK) g_tap_ok++; else g_tap_fail++;
    snprintf(g_last_result, sizeof(g_last_result), "%s %s (%.0f,%.0f)", source, Touch::InjectResultText(r), touch_x, touch_y);
    printf("[盖板] %s\n", g_last_result);
    fflush(stdout);
}

// 界面「测试点击」：交给盖板线程去点(注入要阻塞 hold_ms，不能在渲染线程里做)
static inline void request_test_tap() { g_test_request.store(now_ms()); }

static void thread_main()
{
    std::unordered_map<uint64_t, BoardInfo> infos;
    uint64_t last_board = 0;          // 点过的那块(若辰 [sp+0x70])
    int64_t  last_tap_ms = 0;         // 若辰 [sp+0x40]
    int64_t  last_candidate_ms = 0;
    uint64_t pending = 0;             // 点完等状态变化确认的那块
    int64_t  pending_ms = 0;
    uint32_t rng = (uint32_t)now_ms() | 1;
    int      last_mode = -1;

    while (true) {
        usleep(LOOP_US);

        int64_t t = now_ms();
        int64_t req = g_test_request.exchange(0);
        if (req != 0 && t - req < 1000) do_tap("test");

        if (mode != last_mode) { last_mode = mode; g_half_wid = mode_half_width(mode); }

        int w = 1 - g_rect_active.load();
        g_rect_n[w] = 0;

        if (!enabled && !show_range) {
            snprintf(g_status, sizeof(g_status), "未开启");
            g_rect_n[w] = 0; g_rect_active.store(w);
            usleep(90000);
            continue;
        }

        uint64_t self = 0, hunter = 0;
        if (!PySelf::anchor(self) || PySelf::camp() != 2) {
            snprintf(g_status, sizeof(g_status), "自身不是求生者/未就绪");
            g_rect_active.store(w);
            usleep(90000);
            continue;
        }
        float S[3], H[3];
        if (!read_pos(self, S)) {
            snprintf(g_status, sizeof(g_status), "自身坐标读不到");
            g_rect_active.store(w);
            continue;
        }
        bool have_hunter = PySelf::hunter_body(hunter) && read_pos(hunter, H);

        // 监管种类：小丑拉锯改圆形判定
        // TODO(孽蜥)：若辰对孽蜥整套不触发，我们去掉了。那条规则真正想挡的是"飞天(跳跃)中的孽蜥"，
        //   不是地面上的孽蜥 —— 以后找到跳跃状态的判据(动作号/高度差等)再加回一条只过滤飞天的严格判断
        bool circle = false;
        int action = 0;
        if (have_hunter) {
            std::string cn = class_name_of(hunter);
            if (cn.find("butcher_sxwd") != std::string::npos && read_state(hunter, action))
                circle = action > CLOWN_ACTION - 4 && action < CLOWN_ACTION + 4;
        }

        // 板子：静态信息按对象缓存 2 秒
        int ba = g_board_active.load();
        int nb = g_board_n[ba];
        uint64_t boards[MAX_BOARDS];
        memcpy(boards, g_boards[ba], sizeof(uint64_t) * nb);
        if (infos.size() > 256) infos.clear();

        uint64_t best = 0;
        float best_d = 1e30f;
        int near_n = 0, standing_n = 0;
        for (int i = 0; i < nb; i++) {
            uint64_t o = boards[i];
            auto it = infos.find(o);
            if (it == infos.end() || t - it->second.read_ms > 2000) {
                BoardInfo bi{};
                uint64_t coor = 0;
                if (!read_pos(o, bi.pos, &coor)) { infos.erase(o); continue; }
                float d[2] = {0, 0};
                bi.dir_ok = vm_readv(coor + 0xb8, &d[0], 4) && vm_readv(coor + 0xc0, &d[1], 4)
                         && std::isfinite(d[0]) && std::isfinite(d[1]);
                float len = sqrtf(d[0] * d[0] + d[1] * d[1]);
                if (bi.dir_ok && len > 0.00001f) { bi.dir[0] = d[0] / len; bi.dir[1] = d[1] / len; }
                else bi.dir_ok = false;
                bi.read_ms = t;
                it = infos.insert_or_assign(o, bi).first;
            }
            const BoardInfo &bi = it->second;
            if (hdist(S, bi.pos) > SELF_BOARD_MAX) continue;       // 若辰的预筛([sp+0xcdc])
            near_n++;

            int state = -1;
            bool state_ok = read_state(o, state);
            bool standing = state_ok && state == BOARD_STANDING;
            if (standing) standing_n++;

            bool hit = standing && have_hunter && hit_test(S, H, bi, g_half_wid, circle);
            if (hit) {
                float d = hdist(S, bi.pos);
                if (d < best_d) { best_d = d; best = o; }
            }
            if (show_range && standing && g_rect_n[w] < MAX_RECTS) {
                RangeRect &r = g_rects[w][g_rect_n[w]++];
                r.obj = o;
                r.cx = bi.pos[0]; r.cy = bi.pos[1]; r.cz = bi.pos[2];
                r.ux = bi.dir[0]; r.uy = bi.dir[1];
                r.a = HALF_LEN + MARGIN; r.b = g_half_wid + MARGIN;
                r.r = circle ? CLOWN_RADIUS : 0.f;
                r.candidate = false;
            }
        }
        for (int i = 0; i < g_rect_n[w]; i++) g_rects[w][i].candidate = best != 0 && g_rects[w][i].obj == best;
        g_rect_active.store(w);

        // 点完的那块：状态离开 16 = 放下了
        if (pending) {
            int st = -1;
            if (read_state(pending, st) && st != BOARD_STANDING) {
                g_confirmed++;
                printf("[盖板] 板子状态已变化 obj=0x%llx state=%d 用时=%lldms\n",
                       (unsigned long long)pending, st, (long long)(t - pending_ms));
                pending = 0;
            } else if (t - pending_ms > 3000) {
                pending = 0;                                         // 3 秒没倒，不再跟踪
            }
        }

        if (!enabled) {
            snprintf(g_status, sizeof(g_status), "仅显示范围(未开启自动)");
            continue;
        }
        if (!have_hunter)      { snprintf(g_status, sizeof(g_status), "没有监管"); continue; }
        if (near_n == 0)       { snprintf(g_status, sizeof(g_status), "附近没有板子(板子%d)", nb); last_board = 0; continue; }
        if (standing_n == 0)   { snprintf(g_status, sizeof(g_status), "附近没有立着的板子"); }

        if (best == 0) {
            if (t - last_candidate_ms >= CLEAR_LAST_BOARD_MS) last_board = 0;
            if (standing_n) snprintf(g_status, sizeof(g_status), "监测中%s 监管距离%.1fm",
                                     circle ? "(小丑拉锯·圆形)" : "", hdist(S, H) / dist_scale);
            continue;
        }
        last_candidate_ms = t;
        if (best == last_board) { snprintf(g_status, sizeof(g_status), "这块已经点过"); continue; }
        if (last_tap_ms && t - last_tap_ms < MIN_TAP_INTERVAL_MS) { snprintf(g_status, sizeof(g_status), "冷却中"); continue; }

        last_tap_ms = t;
        // 位置：角度均匀、半径均匀(面积上偏向中心)；时长：hold_ms ± jitter_ms，不低于 10ms
        float tx = touch_x, ty = touch_y;
        if (jitter_px > 0) {
            float ang = frand01(rng) * 6.2831853f, rad = frand01(rng) * (float)jitter_px;
            tx += rad * cosf(ang);
            ty += rad * sinf(ang);
        }
        int hold = hold_ms;
        if (jitter_ms > 0) hold += (int)(xorshift(rng) % (uint32_t)(2 * jitter_ms + 1)) - jitter_ms;
        if (hold < 10) hold = 10;
        int r = Touch::InjectTap(tx, ty, hold);
        if (r == Touch::INJECT_OK) {
            g_tap_ok++;
            last_board = best;
            pending = best;
            pending_ms = t;
        } else {
            g_tap_fail++;
        }
        snprintf(g_last_result, sizeof(g_last_result), "auto %s (%.0f,%.0f) %dms 板子=0x%llx%s",
                 Touch::InjectResultText(r), tx, ty, hold, (unsigned long long)best, circle ? " 圆形" : "");
        printf("[盖板] %s 半宽=%.1f 模式=%d\n", g_last_result, g_half_wid, mode);
        fflush(stdout);
        if (mode == 2) g_half_wid = (8.0f + (float)(xorshift(rng) % 41) * 0.1f) * 0.5f;
        snprintf(g_status, sizeof(g_status), "已点击");
    }
}

static void start()
{
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(thread_main).detach();
}

static inline const char *status_text() { return g_status; }
static inline const char *last_result() { return g_last_result; }

static int ranges(RangeRect *out, int cap)
{
    int a = g_rect_active.load();
    int n = g_rect_n[a] < cap ? g_rect_n[a] : cap;
    memcpy(out, g_rects[a], sizeof(RangeRect) * n);
    return n;
}

} // namespace AutoPallet

#endif
