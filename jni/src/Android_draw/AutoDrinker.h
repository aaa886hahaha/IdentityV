#ifndef IDV_AUTO_DRINKER_H
#define IDV_AUTO_DRINKER_H

// ============================================================================
// 调酒师(DRINKER, pid 91)自动给队友喂酒 —— 2026-11-19 静态调研，skill idv-reverse-resume/drinker.md
// ============================================================================
//
// 按钮出不出、能不能点，全读游戏自己的判定结果，不复刻条件：
//   ui_dict['UIBattle'].inter_widget_info(UIBattleBase 混入了 UIBattleButtonInter) = dict
//   键 "118|<队友uid>"(118 = BATTLE_INTER_UI_INDEX.DRINKER_GIVE_WINE)，值 list：
//   [opt, uid, ui_index, (蒙面角色多一个 icon), [3] 可见, [4] 可点]；隐藏后条目还在、[3]=False。
//   游戏那边已经算好：我有酒、没沉默、队友 fear_num>0 且没倒地、同一世界、可见、距离 ≤20(灰色 ≤50)、没墙挡…
//   队友移动不影响(客户端/服务端都不看)。
// 按钮位置(UIBattleButtonInter.set_widget_pos)：队友模型 'ui' 挂点 cam.world_to_screen 投影；
//   同一队友身上同时可见多个按钮时按 ui_show_priority 横排：第 i 个 x += 屏宽 × 0.1 × cnt(i)，cnt = 0, +1, −1, +2…
//   治疗 11/12 等优先级 1、给酒 118 和 105 优先级 2 → 受伤队友一般同时有治疗按钮，给酒在右边 0.1 屏宽。
//   'ui' 挂点见下面 ui_height 的注释(模型文件里的固定值，2026-11-19 上机确认完全对准)。
// 点击：inter_index_key_code_map[118] = (KEY_CODE_C_GIVE_WINE_DOWN, True) → 按下就触发，点一下即可。
//
// 多个队友同时能喂(用户 2026-11-19 定)：先按血量条件筛(默认 半血及更重 = _fear_num >= fear_injured；可改 任意受伤 = > 0)，
//   再排序：优先队友 > 上椅次数多 > 伤得重 > 离得近。
//   血量：unit.fear_num_component 的 _fear_num / fear_injured(默认 500) / fear_upper(默认 1000)，客户端同步所有人
//   上椅：unit.sit_chair_num(BattleEvent.handle_hook_attack_event 更新，队友也有)
//
// 轮询 16ms(POLL_US)。日志前缀 [给酒]：只记注入失败(2026-11-19 功能确认正常后清掉了其余调试日志)
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <thread>
#include <atomic>
#include <unistd.h>
#include "PyCore.h"
#include "AutoQte.h"
#include "SceneLayout.h"

extern float matrix[16];
extern float px, py;

namespace AutoDrinker {

// ---- 界面配置(渲染线程写、工作线程读) ----
static bool  enabled = true;
static int   hp_mode = 0;              // 0 = 半血及更重(默认)，1 = 任意受伤
static bool  show_touch_point = false;
// 'ui' 挂点：求生者模型 .gim 的 <SocketObject> 里 Name="ui"，BindingBone=""(挂在模型根上、不跟骨骼动画)，
// LocalPosition = (-0.2782, 13.2801, 0.2542)，1401 个求生者 gim 里 1381 个都是这个值(2026-11-19 普查 chr_player.idx)。
// 世界位置 = 模型位置 + 本地坐标 × 模型 3×3(objcoor +0x78 X 轴行 / +0x84 Y 轴行 / +0x90 Z 轴行，含缩放)。
// 用户 2026-11-19 上机手调出来是 13.1(X/Y 0)，跟文件值吻合；滑条改的是本地 Y(高度)，默认取文件值
static float ui_height = 13.2801f;
static const float UI_SOCKET_X = -0.2782f, UI_SOCKET_Z = 0.2542f;
static float off_x = 0.f, off_y = 0.f; // 屏幕像素微调(控件锚点)
static int   hold_ms = 40;
static int   jitter_px = 10;
static std::atomic<int64_t> prefer_uid{0};   // 优先队友(本局 uid，0 = 不指定；换局自动清)
static bool  only_prefer = false;      // 只给优先队友、其他人都不给(默认关；没选人时不生效)

static const int DRINKER_PID = 91;
static const int IDX_WINE = 118;
static const int RETAP_MS = 1500;      // 点完这么久按钮还可点(没喂上)才再点
static const int GAP_MS = 600;         // 两次点击最少间隔
static const int POLL_US = 16000;      // 轮询间隔(用户 2026-11-19 要 16ms，原 30ms)

// 同一队友身上排在给酒前面的按钮：优先级 1 的全部 + 优先级 2 里名字排在 "118|" 前的 105
// (ui_inter_config 展开；排序是 sorted(zip(priority, widget_name))，同优先级按字符串)
static const int PRI_BEFORE_WINE[] = {4, 5, 11, 12, 14, 15, 49, 73, 85, 87, 88, 101, 103, 392, 393, 2020, 2066, 105};

// ---- 发布给渲染线程 ----
static const int MAX_MATE = 8;
struct Mate {
    int64_t uid;
    uint64_t scene;                    // unit.model + 0x20(跟 data[].obj 配对取名字)
    bool wine_vis, wine_on;            // 给酒按钮 可见 / 可点
    int  before;                       // 同一人身上排在给酒按钮前面的可见按钮数(决定横移)
    bool fear_ok; float fear, injured, upper;
    int64_t chairs;                    // -1 = 读不到
    float dist;                        // 世界单位，-1 = 读不到
};
struct View {
    bool active;                       // 我是调酒师且在局内
    int n; Mate m[MAX_MATE];           // 全部队友(不含自己/玩偶/分身)
    int64_t best_uid;                  // 本轮选中的(0 = 没有)
};
static View g_view[2];
static std::atomic<int> g_view_active{0};
static inline View view() { return g_view[g_view_active.load()]; }

static char g_status[160] = "";

using AutoQte::now_us;

static void log_line(const char *s) { printf("[给酒] %s\n", s); fflush(stdout); }

// 第 k 个(从 0 起)按钮的横移系数：游戏 op_flag 从 1 起每次取反，cnt += i * op_flag
static inline int spread_cnt(int k)
{
    int op = 1, cnt = 0;
    for (int i = 0; i <= k; i++) { op = -op; cnt += i * op; }
    return cnt;
}

// 屏幕上的点击位置(我们的叠加层坐标 = 触摸坐标)。sock = ui 挂点世界坐标，mtx 是矩阵快照
static bool tap_point(const float *mtx, float hx, float hy, const float sock[3], int before, float &sx, float &sy)
{
    const float p0 = sock[0], p1 = sock[1], p2 = sock[2];
    float w = mtx[3] * p0 + mtx[7] * p1 + mtx[11] * p2 + mtx[15];
    if (!(w > 0.01f)) return false;
    sx = hx + (mtx[0] * p0 + mtx[4] * p1 + mtx[8] * p2 + mtx[12]) / w * hx;
    sy = hy - (mtx[1] * p0 + mtx[5] * p1 + mtx[9] * p2 + mtx[13]) / w * hy;
    sx += (float)spread_cnt(before) * 0.1f * (2.f * hx) + off_x;
    sy += off_y;
    return std::isfinite(sx) && std::isfinite(sy);
}

// 场景坐标(X / 高度 / Y，跟 Python position 同序)
static bool scene_pos(uint64_t scene, float out[3])
{
    uint64_t c = SceneLayout::coor(scene);
    if (!c || !vm_readv(c + 0xa0, out, 12)) return false;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]) && (out[0] != 0 || out[2] != 0);
}

// ui 挂点世界坐标 = 位置 + (x, 高度, z) × 模型 3×3。3×3 读不出/不像旋转时退回 位置 + (0, 高度, 0)
static bool socket_world(uint64_t scene, float out[3])
{
    uint64_t c = SceneLayout::coor(scene);
    float b[13];                                   // +0x78..+0xab：X 轴行、Y 轴行、Z 轴行、(4 字节空)、位置
    if (!c || !vm_readv(c + 0x78, b, sizeof(b))) return false;
    const float *pos = b + 10;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || (pos[0] == 0 && pos[2] == 0)) return false;
    const float lx = UI_SOCKET_X, ly = ui_height, lz = UI_SOCKET_Z;
    bool rot_ok = true;
    for (int i = 0; i < 9; i++) if (!std::isfinite(b[i]) || fabsf(b[i]) > 10.f) rot_ok = false;
    if (rot_ok) {
        float ylen = sqrtf(b[3] * b[3] + b[4] * b[4] + b[5] * b[5]);
        if (!(ylen > 0.1f && ylen < 10.f)) rot_ok = false;
    }
    for (int k = 0; k < 3; k++)
        out[k] = rot_ok ? pos[k] + lx * b[k] + ly * b[3 + k] + lz * b[6 + k]
                        : pos[k] + (k == 1 ? ly : 0.f);
    return true;
}

// 实例属性所在的槽(dict / split values 都认)，没有返回 0
static uint64_t attr_slot(uint64_t obj, int64_t &idx, const char *name)
{
    PyCore::InstView v;
    if (!PyCore::inst_view(obj, v)) return 0;
    bool ok = false;
    PyCore::inst_attr(v, idx, name, ok);
    if (!ok || idx < 0) return 0;
    return PyCore::inst_value_slot(v, idx);
}
static uint64_t attr(uint64_t obj, int64_t &idx, const char *name)
{
    uint64_t s = attr_slot(obj, idx, name);
    return s ? getPtr64(s) : 0;
}

static bool type_name_has(uint64_t obj, const char *w)
{
    uint64_t tp = PyCore::is_obj(obj) ? getPtr64(obj + 8) : 0;
    uint64_t np = PyCore::is_obj(tp) ? getPtr64(tp + 0x18) : 0;
    char nm[64] = {0};
    if (!PyCore::is_obj(np) || !vm_readv(np, nm, sizeof(nm) - 1)) return false;
    return strstr(nm, w) != nullptr;
}

struct Btn { int64_t uid; bool wine_vis, wine_on; int before; };

// inter_widget_info 里每个队友：给酒按钮状态 + 排在前面的可见按钮数
static bool read_buttons(Btn *out, int cap, int &n, const char *&why)
{
    static thread_local int64_t i_iwi = -1;
    n = 0;
    uint64_t bd = AutoQte::battle_dict(why, false);
    if (!bd) return false;
    uint64_t s = AutoQte::slot_of(bd, i_iwi, "inter_widget_info");
    if (!s) { bd = AutoQte::battle_dict(why, true); s = bd ? AutoQte::slot_of(bd, i_iwi, "inter_widget_info") : 0; }
    if (!s) { if (bd) why = T("UIBattle 上没有 inter_widget_info", "no inter_widget_info on UIBattle"); return false; }
    uint64_t d = getPtr64(s);
    if (!PyCore::is_obj(d) || getPtr64(d + 8) != PyRoot::g_dict_type) { why = T("inter_widget_info 不是 dict", "inter_widget_info not a dict"); return false; }
    PyCore::Dict k;
    if (!PyCore::read_dict(d, k)) { why = T("inter_widget_info 读不出", "inter_widget_info unreadable"); return false; }
    if (k.n_entries > 512) { why = "inter_widget_info too big"; return false; }
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(PyCore::key_slot(k, i));
        if (!PyCore::is_obj(kp)) continue;                       // 已删除的条目
        char key[40];
        if (!PyCore::read_str(kp, key, sizeof(key))) continue;   // 元组键(电梯等)不是 str，跳过
        int idx = 0; long long uid = 0;
        if (sscanf(key, "%d|%lld", &idx, &uid) != 2 || uid <= 0) continue;
        bool is_wine = idx == IDX_WINE, is_before = false;
        for (int b : PRI_BEFORE_WINE) if (b == idx) { is_before = true; break; }
        if (!is_wine && !is_before) continue;
        int64_t ln = 0; uint64_t items = 0;
        if (!PyCore::read_list(getPtr64(PyCore::value_slot(k, i)), ln, items) || ln < 5) continue;
        bool vis = getPtr64(items + 3 * 8) == PyRoot::g_true;
        bool on  = getPtr64(items + 4 * 8) == PyRoot::g_true;
        int j = 0;
        for (; j < n; j++) if (out[j].uid == uid) break;
        if (j == n) { if (n >= cap) continue; out[n] = {uid, false, false, 0}; n++; }
        if (is_wine) { out[j].wine_vis = vis; out[j].wine_on = vis && on; }
        else if (vis) out[j].before++;
    }
    why = "";
    return true;
}

static void thread_main()
{
    uint32_t rng = (uint32_t)now_us() | 1;
    uint64_t last_um = 0;
    int64_t list_ms = -100000;
    struct Unit { int64_t uid; uint64_t obj, scene; };
    Unit units[MAX_MATE]; int un = 0;
    uint64_t my_scene = 0;
    int64_t my_uid = 0;
    int64_t last_tap_ms = -100000, last_tap_uid = 0;

    auto publish_off = [&](const char *st) {
        snprintf(g_status, sizeof(g_status), "%s", st);
        if (g_view[g_view_active.load()].active) {
            int w = 1 - g_view_active.load();
            memset(&g_view[w], 0, sizeof(View));
            g_view_active.store(w);
        }
    };
    auto fail = [&](const char *why, int sleep_ms) {
        publish_off(why);
        usleep(sleep_ms * 1000);
    };

    while (true) {
        if (!enabled) { publish_off(T("未开启", "off")); usleep(200000); continue; }
        if (!PyRoot::g_ready.load()) { fail(T("Python 层未就绪", "Python not ready"), 200); continue; }

        const char *why = "";
        uint64_t md = PyCore::game_kernel_dict(why);
        if (!md) { fail(why, 300); continue; }

        // ---- 我是不是调酒师：GK.g_unit.pid ----
        static thread_local int64_t h_gunit = -1, i_pid = -1, i_uid = -1, i_model = -1;
        h_gunit = PyCore::find_key(md, "g_unit", h_gunit);
        uint64_t gu = h_gunit >= 0 ? PyCore::get_attr(md, h_gunit) : 0;
        int64_t pid = -1;
        if (!PyCore::is_obj(gu) || !PyCore::read_int(attr(gu, i_pid, "pid"), pid)) { fail(T("读不到自己(不在局内?)", "self unreadable (not in match?)"), 300); continue; }
        if (pid != DRINKER_PID) { fail(T("当前不是调酒师", "not playing Bartender"), 500); continue; }
        PyCore::read_int(attr(gu, i_uid, "uid"), my_uid);
        my_scene = PyCore::get_scene_obj(attr(gu, i_model, "model"));

        // ---- 给酒按钮 ----
        Btn btn[MAX_MATE * 2]; int bn = 0;
        if (!read_buttons(btn, MAX_MATE * 2, bn, why)) { fail(why, 300); continue; }

        // ---- 队友列表(units_by_type[2]，500ms 一刷；换局清优先队友) ----
        int64_t tms = now_us() / 1000;
        static thread_local int64_t h_um = -1, h_ubt = -1;
        h_um = PyCore::find_key(md, "unit_mgr", h_um);
        uint64_t um = h_um >= 0 ? PyCore::get_attr(md, h_um) : 0;
        if (um != last_um) {
            if (last_um) prefer_uid.store(0);
            last_um = um; list_ms = -100000; last_tap_uid = 0;
        }
        if (tms - list_ms > 500) {
            list_ms = tms;
            un = 0;
            uint64_t ud = PyCore::get_inst_dict(um);
            if (ud) {
                h_ubt = PyCore::find_key(ud, "units_by_type", h_ubt);
                uint64_t ubt = h_ubt >= 0 ? PyCore::get_attr(ud, h_ubt) : 0;
                uint64_t L = PyCore::is_obj(ubt) ? PyCore::get_type_list(ubt, 2) : 0;
                int64_t ln = 0; uint64_t items = 0;
                if (PyCore::read_list(L, ln, items)) {
                    static thread_local int64_t j_uid = -1, j_model = -1;
                    for (int64_t i = 0; i < ln && un < MAX_MATE; i++) {
                        uint64_t u = getPtr64(items + i * 8);
                        if (!PyCore::is_obj(u) || u == gu) continue;
                        if (type_name_has(u, "Puppet") || type_name_has(u, "Clone")) continue;   // 机械玩偶 / 幻灯师分身
                        int64_t uid = 0;
                        if (!PyCore::read_int(attr(u, j_uid, "uid"), uid) || uid <= 0 || uid == my_uid) continue;
                        units[un++] = {uid, u, PyCore::get_scene_obj(attr(u, j_model, "model"))};
                    }
                }
            }
        }

        // ---- 逐个队友：按钮 + 血量 + 上椅 + 距离 ----
        View v; memset(&v, 0, sizeof(v));
        v.active = true;
        float me[3]; bool me_ok = my_scene && scene_pos(my_scene, me);
        static thread_local int64_t k_fc = -1, k_fear = -1, k_inj = -1, k_up = -1, k_chair = -1;
        for (int i = 0; i < un && v.n < MAX_MATE; i++) {
            Mate &m = v.m[v.n++];
            m.uid = units[i].uid; m.scene = units[i].scene; m.chairs = -1; m.dist = -1;
            for (int j = 0; j < bn; j++) if (btn[j].uid == m.uid) { m.wine_vis = btn[j].wine_vis; m.wine_on = btn[j].wine_on; m.before = btn[j].before; }
            if (!m.wine_vis) continue;                               // 没按钮的不用读细节
            uint64_t fc = attr(units[i].obj, k_fc, "fear_num_component");
            uint64_t sf = attr_slot(fc, k_fear, "_fear_num"), si = attr_slot(fc, k_inj, "fear_injured"), su = attr_slot(fc, k_up, "fear_upper");
            m.fear_ok = sf && PyCore::read_number(sf, m.fear, -1.0, 100000.0);
            if (!si || !PyCore::read_number(si, m.injured, 1.0, 100000.0)) m.injured = 500.f;
            if (!su || !PyCore::read_number(su, m.upper, 1.0, 100000.0)) m.upper = 1000.f;
            if (!PyCore::read_int(attr(units[i].obj, k_chair, "sit_chair_num"), m.chairs)) m.chairs = -1;
            float p[3];
            if (me_ok && m.scene && scene_pos(m.scene, p))
                m.dist = sqrtf((p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]));
        }

        // ---- 选人：可点 + 血量条件，再排序 ----
        const int64_t pref = prefer_uid.load();
        auto qualifies = [&](const Mate &m) {
            if (!m.wine_on) return false;
            if (only_prefer && pref && m.uid != pref) return false;   // 只给固定人
            if (!m.fear_ok) return hp_mode == 1;                    // 读不到血量：任意受伤模式下游戏亮了就算
            return hp_mode == 1 ? m.fear > 0.f : m.fear >= m.injured;
        };
        auto better = [&](const Mate &a, const Mate &b) {            // a 比 b 优先
            bool pa = pref && a.uid == pref, pb = pref && b.uid == pref;
            if (pa != pb) return pa;
            if (a.chairs != b.chairs) return a.chairs > b.chairs;
            float fa = a.fear_ok ? a.fear : 0.f, fb = b.fear_ok ? b.fear : 0.f;
            if (fa != fb) return fa > fb;
            float da = a.dist < 0 ? 1e9f : a.dist, db = b.dist < 0 ? 1e9f : b.dist;
            return da < db;
        };
        int best = -1;
        for (int i = 0; i < v.n; i++) if (qualifies(v.m[i]) && (best < 0 || better(v.m[i], v.m[best]))) best = i;
        v.best_uid = best >= 0 ? v.m[best].uid : 0;

        int w = 1 - g_view_active.load();
        g_view[w] = v;
        g_view_active.store(w);

        if (best < 0) {
            int vis = 0; for (int i = 0; i < v.n; i++) vis += v.m[i].wine_vis;
            snprintf(g_status, sizeof(g_status), "%s", vis ? T("有给酒按钮，但不可点或不满足血量条件", "button shown but not tappable / HP rule not met")
                                                           : T("待命(没有给酒按钮)", "idle (no give-wine button)"));
            usleep(POLL_US);
            continue;
        }

        // ---- 点击 ----
        const Mate &t = v.m[best];
        tms = now_us() / 1000;
        if (tms - last_tap_ms < GAP_MS || (t.uid == last_tap_uid && tms - last_tap_ms < RETAP_MS)) {
            snprintf(g_status, sizeof(g_status), "%s", T("已点击，等按钮消失", "tapped, waiting"));
            usleep(POLL_US);
            continue;
        }
        float tp[3];
        float mtx[16], mtx2[16], hx = px, hy = py;
        bool got = false;
        for (int r = 0; r < 3 && !got; r++) {                        // 渲染线程每帧整块重写矩阵：两次读一样才用
            memcpy(mtx, matrix, sizeof(mtx)); memcpy(mtx2, matrix, sizeof(mtx2));
            got = memcmp(mtx, mtx2, sizeof(mtx)) == 0;
        }
        float sx, sy;
        if (!got || !t.scene || !socket_world(t.scene, tp) || !tap_point(mtx, hx, hy, tp, t.before, sx, sy)
            || sx < 0 || sy < 0 || sx > 2.f * hx || sy > 2.f * hy) {
            snprintf(g_status, sizeof(g_status), "%s", T("算不出按钮位置(队友不在屏幕内?)", "can't place the button (mate off screen?)"));
            usleep(POLL_US);
            continue;
        }
        if (jitter_px > 0) {
            float ang = AutoQte::frand01(rng) * 6.2831853f, rad = AutoQte::frand01(rng) * (float)jitter_px;
            sx += rad * cosf(ang); sy += rad * sinf(ang);
        }
        int r = Touch::InjectTap(sx, sy, hold_ms < 10 ? 10 : hold_ms);
        last_tap_ms = tms; last_tap_uid = t.uid;
        if (r != Touch::INJECT_OK) {                                  // 只记注入失败
            char b[128];
            snprintf(b, sizeof(b), "注入失败 %s (%.0f,%.0f)", Touch::InjectResultText(r), sx, sy);
            log_line(b);
        }
        snprintf(g_status, sizeof(g_status), "%s", T("已点击", "tapped"));
        usleep(POLL_US);
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

} // namespace AutoDrinker

#endif
