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
//   监管   PySelf::hunter_bodies()(联合狩猎 2 个)；动作号同样是 [[obj+0x750]+0x30]
//          照游戏 is_butcher_can_hit：梦之女巫本体不算、换成她正在操控的信徒；约瑟夫镜世界只算跟我同一世界的；
//          super_armor(霸体：兴奋/破轮轮形态/台球手球形态)不判
//
// 二、命中判定 sub_34517C(自身 S, 监管 H, 板子 P，全部只看水平面)
//   state == 16
//   |S-H| / 11.886 <= 3 米
//   |S-P| <= 25 单位(约 2.1 米)                       —— 自己要站在板子边上才放得下
//   普通：把 H-P 投到板子朝向 d 上
//         |along| <= 8.25 + 0.38  且  |perp| <= 半宽 + 0.38
//   小丑拉锯(类名含 butcher_sxwd 且动作号在 0x1802CE±3)：改成 |H-P| <= 80 单位(约 6.7 米)，不需要朝向
//   多块同时命中时取离自己最近的那块
//   若辰在监管是孽蜥时整套不触发(按翻译名"孽蜥"判)；我们只挡空中的：监管与自身高度差 > 约 1.26 米不判(见 hit_test)
//   破轮轮形态(模型 xxx_wheel.gim)不放板
//
// 三、半宽(界面「触发宽度」，游戏单位，判定用 宽度/2)
//   固定：宽度 = width(默认 12，若辰「暴力」档)
//   随机：每次点完在 [width_lo, width_hi] 之间(步长 0.1)重抽。两种都限制在 5~15
//   (若辰原来的三档：暴力 12 / 演戏 8 / 随机 8~12，都能用这两种方式表示)
//
// 四、门控
//   同一块板点过一次就不再点，直到连续 150ms 没有任何候选(监管离开了判定区)
//   两次点击至少间隔 800ms(失败也算一次)
//   点完记下这块板，等它的状态从 16 变成别的值 = 确认放下(只用于统计/面板显示)
//
// 五、跟若辰的不同(有意的)
//   - 若辰还会合并 Python 侧的板子列表(sub_38F028，3 秒内有效)；这里只用场景对象
//   - 若辰对全部监管取最近的一个；这里对每个监管都判，任一命中就算(联合狩猎两个监管)
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
static float width = 12.0f;                // 固定模式的触发宽度(游戏单位)
static bool  random_width = false;         // 随机模式：每次点完在下限~上限之间重抽
static float width_lo = 8.0f, width_hi = 12.0f;
static const float WIDTH_MIN = 5.0f, WIDTH_MAX = 15.0f;
// 放行：只在游戏主交互按钮此刻就是"放板"时才点(UIBattle.cbtn_alpha_st == 3，见 PyCore::read_inter_button)。
// 能不能放(三维距离 ≤ 25、板立着、无遮挡、单向板朝向…)交给游戏判，我们只判"什么时候放"(监管进砸中区)；
// 顺带挡住"按钮此刻是翻窗/救人/开箱"时误点成别的动作。3 = PUT_PANEL 于 2026-10-23 国际服上机日志确认。
static const int64_t ALPHA_PUT_PANEL = 3;
// 快速监管预判：监管实测速度超过门槛时，把他的位置往前推 lead_ms，推出来的点落进判定区也算命中(提前点)。
// 门槛依据：监管配表移速 pydata/player.move_speed 绝大多数 54~56 单位/秒(4.54~4.71 m/s，杰克 56)，
// 运行时 = base_speed × 状态倍率 × buff(common_cs/mgr/MoveSpeedMgr.speed_real)，所以超过 ×1.25 基本就是技能冲刺/加速。
// 正常速度走近停住的监管到不了门槛，只走"已进区才点"那条，不会被预判骗板。
// 提前量是时间(≈端到端延迟 L：远端插值落后 + 本轮检测 + 注入 + 网络 + 服务端起倒板)，与速度无关。
// 但要砸中需 0 ≤ L − 提前量 ≤ 穿区时间(区深/速度)：越快容错越小。点早了不要紧——游戏倒板后 1.1s 内一直检测，
// 冲刺中的监管照样冲进来；点晚了就穿过去了。所以宁大勿小，默认 350ms。
static bool  predict_fast = true;
static float fast_mps = 5.9f;              // 预判速度门槛(米/秒) = 4.71 × 1.25
static int   lead_ms = 350;
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
static const float HEIGHT_MAX        = 15.0f;   // 监管与自身的高度差上限(约 1.26 米)，见 hit_test

static inline float clamp_width(float w) { return w < WIDTH_MIN ? WIDTH_MIN : (w > WIDTH_MAX ? WIDTH_MAX : w); }

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
static char g_status[160] = "";   // 空 = 未开启，见 status_text()
static std::atomic<int> g_tap_ok{0}, g_tap_fail{0}, g_confirmed{0};
static char g_last_result[128] = "-";
static float g_half_wid = 6.0f;            // 本轮半宽
static std::atomic<int64_t> g_test_request{0};
// 游戏主交互按钮状态(盖板线程独占读写)
static int64_t g_btn_st = -1;

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
// 比若辰多一道高度差：游戏 is_unit_in_hit_range 用三维距离(x² = dist² − y² < 12²)，监管离地约 1 米以上就砸不中。
// 为的是孽蜥起跳/滞空(脚本 skills/wildman/jump，孽蜥代号 wildman、模型 lizard)。拿自身脚底比(自己就站在板边)
static bool hit_test(const float *S, const float *H, const BoardInfo &b, float half_wid, bool circle)
{
    if (hdist(S, H) / dist_scale > HUNTER_SELF_MAX_M) return false;
    if (fabsf(H[2] - S[2]) > HEIGHT_MAX) return false;
    if (hdist(S, b.pos) > SELF_BOARD_MAX) return false;
    if (circle) return hdist(H, b.pos) <= CLOWN_RADIUS;
    if (!b.dir_ok) return false;
    float dx = H[0] - b.pos[0], dy = H[1] - b.pos[1];
    float along = fabsf(dx * b.dir[0] + dy * b.dir[1]);
    float perp  = fabsf(b.dir[0] * dy - b.dir[1] * dx);
    if (along > HALF_LEN + MARGIN) return false;
    return perp <= half_wid + MARGIN;
}

// ---- 监管速度估计(盖板线程独占，每个监管一份：联合狩猎两个监管共用一份会互相清空) ----
// 每轮记一个水平位置样本，取最近 VEL_WINDOW_MS 内的样本做最小二乘，得到速度向量(单位/秒)。
// 传送判据用"单次跳变距离 > TELEPORT_JUMP_M"，**不能用相邻样本换算的速度**：远端位置随网络包阶梯式更新
// (几轮不动、然后一跳)，冲刺越快那一跳换算出的速度越高，旧版"> 30m/s 算传送"会把快速冲刺误清掉，正好越快越漏。
struct HunterSample { int64_t t; float x, y; };
static const int   HIST_CAP = 32;
static const int64_t VEL_WINDOW_MS = 80;
static const float TELEPORT_JUMP_M = 5.0f;
struct Track { uint64_t obj; HunterSample hist[HIST_CAP]; int n; int64_t seen_ms; };
static Track g_tracks[PySelf::MAX_HUNTERS];

// 按监管对象取它的历史；没有就占用最久没见到的那格(换局/换人自然顶掉)
static Track &track_of(uint64_t obj, int64_t t)
{
    Track *old = &g_tracks[0];
    for (Track &k : g_tracks) {
        if (k.obj == obj) { k.seen_ms = t; return k; }
        if (k.seen_ms < old->seen_ms) old = &k;
    }
    old->obj = obj; old->n = 0; old->seen_ms = t;
    return *old;
}

static void hunter_track(Track &k, int64_t t, const float *H)
{
    if (k.n > 0) {
        const HunterSample &p = k.hist[k.n - 1];
        float dt = (float)(t - p.t) / 1000.f;
        if (dt <= 0.f) return;
        float dx = H[0] - p.x, dy = H[1] - p.y;
        if (dt > 0.25f || sqrtf(dx * dx + dy * dy) / dist_scale > TELEPORT_JUMP_M) k.n = 0;   // 断档或传送
    }
    if (k.n == HIST_CAP) { memmove(k.hist, k.hist + 1, sizeof(HunterSample) * (HIST_CAP - 1)); k.n--; }
    k.hist[k.n++] = {t, H[0], H[1]};
}

// 返回 false = 样本不够(少于 3 个或跨度不到 30ms，冲刺刚起步也要尽快出结果)。
// decel = 后半段速度不到前半段 50%(在刹车)；两半各自不到 30ms 时阶梯式更新会让端点速度乱跳，不做判断
static bool hunter_velocity(const Track &k, int64_t now, float &vx, float &vy, bool &decel)
{
    const HunterSample *h = k.hist;
    int first = k.n;
    while (first > 0 && now - h[first - 1].t <= VEL_WINDOW_MS) first--;
    int n = k.n - first;
    if (n < 3 || h[k.n - 1].t - h[first].t < 30) return false;
    double st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
    for (int i = first; i < k.n; i++) {
        double tt = (double)(h[i].t - h[first].t) / 1000.0;
        st += tt; sx += h[i].x; sy += h[i].y; stt += tt * tt; stx += tt * h[i].x; sty += tt * h[i].y;
    }
    double den = n * stt - st * st;
    if (den <= 1e-9) return false;
    vx = (float)((n * stx - st * sx) / den);
    vy = (float)((n * sty - st * sy) / den);
    // 前后两半各用端点算平均速度
    int mid = first + n / 2;
    auto seg_speed = [](const HunterSample &a, const HunterSample &b) {
        float dt = (float)(b.t - a.t) / 1000.f;
        if (dt <= 0.f) return 0.f;
        float dx = b.x - a.x, dy = b.y - a.y;
        return sqrtf(dx * dx + dy * dy) / dt;
    };
    decel = false;
    if (h[mid].t - h[first].t >= 30 && h[k.n - 1].t - h[mid].t >= 30) {
        float early = seg_speed(h[first], h[mid]), late = seg_speed(h[mid], h[k.n - 1]);
        decel = early > 0.f && late < early * 0.5f;
    }
    return true;
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

// 随机模式：在 [下限, 上限] 之间按 0.1 步长抽一个宽度，返回半宽。下限 > 上限时按两者中小的为下限
static float pick_random_half_width(uint32_t &rng)
{
    float lo = clamp_width(width_lo), hi = clamp_width(width_hi);
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    int steps = (int)lroundf((hi - lo) * 10.0f);
    float w = lo + (float)(steps > 0 ? xorshift(rng) % (uint32_t)(steps + 1) : 0) * 0.1f;
    return w * 0.5f;
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
    bool     last_random = false;
    float    last_lo = -1.f, last_hi = -1.f;

    while (true) {
        usleep(LOOP_US);

        int64_t t = now_ms();
        int64_t req = g_test_request.exchange(0);
        if (req != 0 && t - req < 1000) do_tap("test");

        // 固定模式每轮跟着滑块走；随机模式只在开关/上下限变了时重抽(点完一次也会重抽，见末尾)
        if (!random_width) {
            g_half_wid = clamp_width(width) * 0.5f;
        } else if (!last_random || width_lo != last_lo || width_hi != last_hi) {
            g_half_wid = pick_random_half_width(rng);
        }
        last_random = random_width; last_lo = width_lo; last_hi = width_hi;

        int w = 1 - g_rect_active.load();
        g_rect_n[w] = 0;

        if (!enabled && !show_range) {
            snprintf(g_status, sizeof(g_status), "%s", T("未开启", "off"));
            g_rect_n[w] = 0; g_rect_active.store(w);
            usleep(20000);
            continue;
        }

        // 游戏主交互按钮(只有开着自动才需要)：每轮读，读成功才更新；偶发读失败(页换出/对象重建)沿用上一次的值，
        // 连续失败超过 BTN_HOLD_MS 才清空，免得一次失败就让"是不是放板"闪掉而漏点。"没有 UIBattle"是真的不在局内，立刻清空。
        // 延迟：游戏自己的放板检查每 ui_update_button_inv 帧才跑一次(UIBattleButton.tick_btn_alpha 的完整检查组)，
        // 屏幕上的按钮也是同一个值，手动按也受同样的延迟，这层绕不过去(2026-10-23 日志估 0.1~0.2s)
        if (enabled) {
            static int64_t last_ok_ms = 0;
            static const int64_t BTN_HOLD_MS = 300;
            PyCore::InterButton ib;
            const char *why = "";
            if (PyRoot::g_ready.load() && PyCore::read_inter_button(ib, why)) {
                last_ok_ms = t;
                g_btn_st = ib.alpha_st;
            } else {
                bool not_in_battle = strstr(why, "不在局内") || strstr(why, "not in match");   // PyCore::read_inter_button 的那条原因
                if (not_in_battle || t - last_ok_ms > BTN_HOLD_MS) g_btn_st = -1;
            }
        }

        uint64_t self = 0;
        if (!PySelf::anchor(self) || PySelf::camp() != 2) {
            snprintf(g_status, sizeof(g_status), "%s", T("自身不是求生者/未就绪", "self not survivor / not ready"));
            g_rect_active.store(w);
            usleep(20000);
            continue;
        }
        float S[3];
        if (!read_pos(self, S)) {
            snprintf(g_status, sizeof(g_status), "%s", T("自身坐标读不到", "self position unreadable"));
            g_rect_active.store(w);
            continue;
        }

        // 监管：联合狩猎(2v8)有两个，逐个读坐标、测速，下面对每块板任一监管命中就算
        // 霸体(super_armor)的照样读坐标测速(霸体结束时速度现成)，只是不参与命中：游戏 is_butcher_can_hit 直接不判
        struct Hunter { float H[3]; float vx, vy, mps; bool pred_on, circle, armor; };
        PySelf::HunterTarget tgs[PySelf::MAX_HUNTERS];
        uint64_t bodies[PySelf::MAX_HUNTERS];
        int nbody = PySelf::hunter_targets(tgs, PySelf::MAX_HUNTERS);
        for (int i = 0; i < nbody; i++) bodies[i] = tgs[i].obj;
        Hunter hs[PySelf::MAX_HUNTERS];
        int nh = 0, nearest = -1;
        bool wheel_form = false;
        for (int i = 0; i < nbody; i++) {
            Hunter &h = hs[nh];
            if (!read_pos(bodies[i], h.H)) continue;
            h.armor = tgs[i].armor;
            // 破轮轮形态不放板：ButcherUnit._recover_broken_wheel 在 broken_wheel 为真时 _change_another_model，
            // 当前 model 换成 xxx_wheel.gim(每款皮肤都有一个；polun_e_pzz_wheel_dingzi 是钉子不是本体)
            std::string cn = class_name_of(bodies[i]);
            if (cn.find("polun") != std::string::npos && cn.find("wheel") != std::string::npos
                && cn.find("dingzi") == std::string::npos) { wheel_form = true; continue; }
            // 快速监管预判：速度过门槛、没在刹车，才把位置往前推(见 predict_fast 的说明)
            Track &k = track_of(bodies[i], t);
            hunter_track(k, t, h.H);
            h.vx = h.vy = h.mps = 0.f;
            h.pred_on = false;
            bool decel = false;
            if (hunter_velocity(k, t, h.vx, h.vy, decel)) {
                h.mps = sqrtf(h.vx * h.vx + h.vy * h.vy) / dist_scale;
                h.pred_on = predict_fast && !decel && h.mps > fast_mps;
            }
            // 小丑拉锯改圆形判定
            int action = 0;
            h.circle = cn.find("butcher_sxwd") != std::string::npos && read_state(bodies[i], action)
                    && action > CLOWN_ACTION - 4 && action < CLOWN_ACTION + 4;
            if (nearest < 0 || hdist(S, h.H) < hdist(S, hs[nearest].H)) nearest = nh;
            nh++;
        }
        bool any_circle = false, any_pred = false;
        for (int i = 0; i < nh; i++) { any_circle |= hs[i].circle; any_pred |= hs[i].pred_on; }

        // 板子：静态信息按对象缓存 2 秒
        int ba = g_board_active.load();
        int nb = g_board_n[ba];
        uint64_t boards[MAX_BOARDS];
        memcpy(boards, g_boards[ba], sizeof(uint64_t) * nb);
        if (infos.size() > 256) infos.clear();

        uint64_t best = 0;
        float best_d = 1e30f;
        bool  best_pred = false;                     // 命中来自预判(监管还没进区)
        int   armor_h = -1;                          // 有霸体监管站在某块板的判定区里(不放，状态行提示)
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

            bool hit = false, hit_pred = false;
            for (int h = 0; standing && h < nh && !hit; h++) {
                const Hunter &hu = hs[h];
                if (hu.armor) {                                      // 霸体：不判，只记一下"在区里"给状态行
                    if (armor_h < 0 && hit_test(S, hu.H, bi, g_half_wid, hu.circle)) armor_h = h;
                    continue;
                }
                hit = hit_test(S, hu.H, bi, g_half_wid, hu.circle);
                if (hit || hit_pred || !hu.pred_on) continue;
                // 每 5ms 一个预测点，推到 lead_ms 为止(20ms 步长在高速下一步近半个判定区，会跳过边角)；高度沿用当前值
                for (int dt = 5; dt <= lead_ms && !hit_pred; dt += 5) {
                    float P[3] = {hu.H[0] + hu.vx * dt / 1000.f, hu.H[1] + hu.vy * dt / 1000.f, hu.H[2]};
                    hit_pred = hit_test(S, P, bi, g_half_wid, hu.circle);
                }
            }
            if (hit || hit_pred) {
                float d = hdist(S, bi.pos);
                if (d < best_d) { best_d = d; best = o; best_pred = !hit; }
            }
            if (show_range && standing && g_rect_n[w] < MAX_RECTS) {
                RangeRect &r = g_rects[w][g_rect_n[w]++];
                r.obj = o;
                r.cx = bi.pos[0]; r.cy = bi.pos[1]; r.cz = bi.pos[2];
                r.ux = bi.dir[0]; r.uy = bi.dir[1];
                r.a = HALF_LEN + MARGIN; r.b = g_half_wid + MARGIN;
                r.r = any_circle ? CLOWN_RADIUS : 0.f;
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
            snprintf(g_status, sizeof(g_status), "%s", T("仅显示范围(未开启自动)", "range only (auto off)"));
            continue;
        }
        if (nh == 0) {
            snprintf(g_status, sizeof(g_status), "%s", wheel_form ? T("破轮轮形态(不放板)", "Breaking Wheel in wheel form (no pallet)") : T("没有监管", "no hunter"));
            continue;
        }
        if (near_n == 0)       { snprintf(g_status, sizeof(g_status), T("附近没有板子(板子%d)", "no pallet nearby (pallets %d)"), nb); last_board = 0; continue; }
        if (standing_n == 0)   { snprintf(g_status, sizeof(g_status), "%s", T("附近没有立着的板子", "no standing pallet nearby")); }

        if (best == 0) {
            if (t - last_candidate_ms >= CLEAR_LAST_BOARD_MS) last_board = 0;
            if (armor_h >= 0) snprintf(g_status, sizeof(g_status), "%s", T("霸体监管在判定区(不放板)", "super-armored hunter in range (no pallet)"));
            else if (standing_n) snprintf(g_status, sizeof(g_status), T("监测中%s 监管%d 最近%.1fm 速度%.1fm/s%s", "watching%s hunters %d nearest %.1fm %.1fm/s%s"),
                                     any_circle ? T("(小丑拉锯·圆形)", " (Smiley Face dash, circle)") : "", nh,
                                     hdist(S, hs[nearest].H) / dist_scale, hs[nearest].mps, any_pred ? T(" 预判中", " predicting") : "");
            continue;
        }
        last_candidate_ms = t;
        if (g_btn_st != ALPHA_PUT_PANEL) {
            snprintf(g_status, sizeof(g_status), T("命中但游戏按钮不是放板(%lld)", "hit, but game button is not put-panel (%lld)"),
                     (long long)g_btn_st);
            continue;
        }
        if (best == last_board) { snprintf(g_status, sizeof(g_status), "%s", T("这块已经点过", "already tapped this one")); continue; }
        if (last_tap_ms && t - last_tap_ms < MIN_TAP_INTERVAL_MS) { snprintf(g_status, sizeof(g_status), "%s", T("冷却中", "cooling down")); continue; }

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
        snprintf(g_last_result, sizeof(g_last_result), T("auto %s (%.0f,%.0f) %dms 板子=0x%llx%s%s %.1fm/s", "auto %s (%.0f,%.0f) %dms pallet=0x%llx%s%s %.1fm/s"),
                 Touch::InjectResultText(r), tx, ty, hold, (unsigned long long)best, any_circle ? T(" 圆形", " circle") : "",
                 best_pred ? T(" 预判", " predicted") : "", hs[nearest].mps);
        printf("[盖板] %s 半宽=%.1f 随机=%d\n", g_last_result, g_half_wid, (int)random_width);
        fflush(stdout);
        if (random_width) g_half_wid = pick_random_half_width(rng);
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
