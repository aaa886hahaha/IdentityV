#pragma once
// ============================================================================
// PyMadEye.h —— 疯眼控制台第一层地图(UIBattleMonitorTop)：把求生者画到游戏自己的俯视地图上
// ============================================================================
//
// 来历：2026-11-15 静态反编译 + 离线解 csb(battle_monitor_controller.csb)，完整记录见 skill idv-reverse-resume/madeye.md。
// 游戏自己的地图只画密码机/挂人椅子/控制台/自己，不画求生者 —— 这里补上。
//
// 换算(UIBattleMonitorTop.world_pos_to_cocos_pos + csb 布局 + UIManager 全面屏适配，全部可从内存读)：
//   d      = map_scale × ((x,z) − map_center)，再按 map_z_flag 转(照抄游戏，DOWN 那个分支看着怪但就是这么写的)：
//            UP(1) 不变 / RIGHT(2) (d.y, −d.x) / DOWN(3) (−d.y, −d.x) / LEFT(4) (−d.y, d.x)
//   根节点 = need ? (g_design_size × pos_ratio, g_design_size × size_ratio) : ((0,0), g_design_size)
//            need = g_ui_mgr.need_full_screen_phone_adapt && 这个 UI 的 added_to_layer
//   center = 根位置 + 0.5 × 根尺寸                       (p_monitor_controller 按 50%/50% 放、锚点居中)
//   design = center + 0.95 × ((0,20) + d)                 (面板缩放 0.95；img_map 在面板里 (100,120)，锚点 (100,100))
//   屏幕   = (design.x × k, 屏高 − design.y × k)，k = 屏宽 / g_design_size.w   (cocos y 朝上)
// 地图开着 = widget_operators_dict 非空(hide() 一开始就清空它；推断，待上机，日志里有)。
//
// 线程：100ms 一轮，只在开关开着且局内时读。绘制侧每帧取快照自己算屏幕坐标(求生者坐标用场景层的，跟方框同源)。
// 不确定的地方(根节点父层有没有额外偏移/缩放、k 横竖是否一致、地窖楼层)都打日志，见 log_state()。

#include <atomic>
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "PyCore.h"
#include "PyScene.h"

namespace PyMadEye {

struct MapState {
    bool  open = false;                 // 第一层地图正在显示
    float cx = 0, cz = 0, scale = 0;    // map_center / map_scale
    int   zflag = 1;                    // map_z_flag：1上 2右 3下 4左
    float dw = 0, dh = 0;               // GK.g_design_size
    float root_x = 0, root_y = 0, root_w = 0, root_h = 0;   // 界面根节点(设计坐标)
    bool  need_adapt = false;
    int   floor = -1, map_id = -1, n_ops = 0;               // 日志用
};

inline bool enabled = true;                     // 菜单开关(配置键 madeye_map)
static MapState g_state[2];
static std::atomic<int> g_active{0};
static uintptr_t g_libbase = 0;
static char g_status[128] = "";

using PyCore::is_obj; using PyCore::Dict; using PyCore::find_key; using PyCore::get_attr;
using PyCore::read_number; using PyCore::read_int; using PyCore::read_bool;

// 二元组(数字) —— map_center / g_design_size 都是 Python tuple：+0x10 长度，+0x18 起元素指针
static bool read_pair(uint64_t tup, float &a, float &b, double lo, double hi)
{
    if (!is_obj(tup)) return false;
    int64_t n = 0;
    if (!vm_readv(tup + 0x10, &n, 8) || n != 2) return false;
    return read_number(tup + 0x18, a, lo, hi) && read_number(tup + 0x20, b, lo, hi);
}

// 实例属性：值槽地址(给 read_number/read_bool 用)，没有这个属性返回 0
static uint64_t attr_slot(const PyCore::InstView &v, int64_t &idx, const char *name)
{
    bool ok = false;
    PyCore::inst_attr(v, idx, name, ok);
    return (ok && idx >= 0) ? PyCore::inst_value_slot(v, idx) : 0;
}

static void log_state(const MapState &s, const char *why)
{
    printf("[疯眼地图] %s 开=%d 图=%d 层=%d 控制台图标=%d 中心=(%.1f,%.1f) 比例=%.4f 朝向=%d 设计=%.1fx%.1f "
           "全面屏适配=%d 根=(%.1f,%.1f %.1fx%.1f)\n",
           why, s.open, s.map_id, s.floor, s.n_ops, s.cx, s.cz, s.scale, s.zflag, s.dw, s.dh,
           s.need_adapt, s.root_x, s.root_y, s.root_w, s.root_h);
    fflush(stdout);
}

static bool refresh_once(MapState &s)
{
    static thread_local int64_t h_uimgr = -1, h_ds = -1, h_uidict = -1, h_top = -1;
    static thread_local int64_t h_need = -1, h_px = -1, h_py = -1, h_sx = -1, h_sy = -1;
    static thread_local int64_t h_center = -1, h_scale = -1, h_zf = -1, h_ops = -1, h_added = -1, h_floor = -1, h_mapid = -1;
    s = MapState();
    const char *why = "";
    uint64_t md = PyCore::game_kernel_dict(why);
    if (!md) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }

    h_ds = find_key(md, "g_design_size", h_ds);
    if (h_ds < 0 || !read_pair(get_attr(md, h_ds), s.dw, s.dh, 100.0, 10000.0)) {
        snprintf(g_status, sizeof(g_status), "%s", T("读不到 g_design_size", "g_design_size unreadable")); return false;
    }
    h_uimgr = find_key(md, "g_ui_mgr", h_uimgr);
    uint64_t mgr = h_uimgr >= 0 ? get_attr(md, h_uimgr) : 0;
    PyCore::InstView mv;
    if (!PyCore::inst_view(mgr, mv)) { snprintf(g_status, sizeof(g_status), "%s", T("g_ui_mgr 无效", "g_ui_mgr invalid")); return false; }

    bool ok = false;
    uint64_t uidict = PyCore::inst_attr(mv, h_uidict, "ui_dict", ok);
    if (!ok) { snprintf(g_status, sizeof(g_status), "%s", T("没有 ui_dict", "no ui_dict")); return false; }
    h_top = find_key(uidict, "UIBattleMonitorTop", h_top);
    if (h_top < 0) { snprintf(g_status, sizeof(g_status), "%s", T("地图界面没创建(没开过控制台)", "map UI not created")); return true; }
    PyCore::InstView tv;
    if (!PyCore::inst_view(get_attr(uidict, h_top), tv)) { snprintf(g_status, sizeof(g_status), "%s", T("地图界面实例无效", "map UI instance invalid")); return false; }

    // 地图开着：widget_operators_dict 非空(PyDictObject.ma_used 在 +0x10)
    uint64_t ops = PyCore::inst_attr(tv, h_ops, "widget_operators_dict", ok);
    int64_t used = 0;
    if (ok && is_obj(ops)) vm_readv(ops + 0x10, &used, 8);
    s.n_ops = (int)used;

    uint64_t sl;
    if (!(sl = attr_slot(tv, h_scale, "map_scale")) || !read_number(sl, s.scale, 0.001, 100.0)) {
        snprintf(g_status, sizeof(g_status), "%s", T("还没填过地图(无 map_scale)", "map not filled yet")); return true;
    }
    uint64_t center = PyCore::inst_attr(tv, h_center, "map_center", ok);
    if (!ok || !read_pair(center, s.cx, s.cz, -1e6, 1e6)) { snprintf(g_status, sizeof(g_status), "%s", T("map_center 读不到", "map_center unreadable")); return false; }
    int64_t iv = 1;
    if ((sl = attr_slot(tv, h_zf, "map_z_flag")) && read_int(getPtr64(sl), iv) && iv >= 1 && iv <= 4) s.zflag = (int)iv;
    if ((sl = attr_slot(tv, h_floor, "cur_floor")) && read_int(getPtr64(sl), iv)) s.floor = (int)iv;
    if ((sl = attr_slot(tv, h_mapid, "photo_map_id")) && read_int(getPtr64(sl), iv)) s.map_id = (int)iv;

    // 全面屏适配：need = 管理器开关 && 这个 UI added_to_layer(读不到按 false，日志里能看出来)
    bool need = false, added = false;
    if ((sl = attr_slot(mv, h_need, "need_full_screen_phone_adapt"))) read_bool(sl, need);
    if ((sl = attr_slot(tv, h_added, "added_to_layer"))) read_bool(sl, added);
    s.need_adapt = need && added;
    s.root_w = s.dw; s.root_h = s.dh;
    if (s.need_adapt) {
        float px = 0, py = 0, sx = 1, sy = 1;
        if ((sl = attr_slot(mv, h_px, "full_screen_phone_pos_ratio_x")))  read_number(sl, px, 0.0, 1.0);
        if ((sl = attr_slot(mv, h_py, "full_screen_phone_pos_ratio_y")))  read_number(sl, py, 0.0, 1.0);
        if ((sl = attr_slot(mv, h_sx, "full_screen_phone_size_ratio_x"))) read_number(sl, sx, 0.1, 1.0);
        if ((sl = attr_slot(mv, h_sy, "full_screen_phone_size_ratio_y"))) read_number(sl, sy, 0.1, 1.0);
        s.root_x = s.dw * px; s.root_y = s.dh * py; s.root_w = s.dw * sx; s.root_h = s.dh * sy;
    }
    s.open = s.n_ops > 0;
    snprintf(g_status, sizeof(g_status), T("%s 图%d 比例%.3f", "%s map%d scale%.3f"),
             s.open ? T("地图开着", "map open") : T("地图关着", "map closed"), s.map_id, s.scale);
    return true;
}

static void thread_main()
{
    MapState last;
    bool last_ok = false;
    while (true) {
        if (!enabled || !PyScene::in_battle() || !PyRoot::ensure()) {
            int w = 1 - g_active.load();
            g_state[w] = MapState();
            g_active = w;
            if (!enabled) snprintf(g_status, sizeof(g_status), "%s", T("未开启", "off"));
            else if (!PyScene::in_battle()) snprintf(g_status, sizeof(g_status), "%s", T("不在局内", "not in match"));
            last_ok = false;
            usleep(200 * 1000);
            continue;
        }
        MapState s;
        bool ok = refresh_once(s);
        if (!ok) s = MapState();
        int w = 1 - g_active.load();
        g_state[w] = s;
        g_active = w;
        // 开/关切换、换图、换层、参数变了时打一行(地图开着时控制台图标数变化不算)
        if (ok && (!last_ok || s.open != last.open || s.map_id != last.map_id || s.floor != last.floor
                   || s.scale != last.scale || s.cx != last.cx || s.cz != last.cz || s.dw != last.dw
                   || s.need_adapt != last.need_adapt || s.root_x != last.root_x || s.root_y != last.root_y))
            log_state(s, s.open ? "打开" : "状态");
        last = s; last_ok = ok;
        usleep(100 * 1000);
    }
}

static void start(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase = libbase;
    snprintf(g_status, sizeof(g_status), "%s", T("启动中", "starting"));
    std::thread(thread_main).detach();
    printf("[疯眼地图] 已启动\n");
    fflush(stdout);
}

// ---------------- 绘制侧 ----------------
static inline MapState state() { return g_state[g_active.load()]; }
static inline const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

// 世界 (x, z) -> 屏幕像素。W/H = 叠加层屏幕尺寸
static inline void to_screen(const MapState &s, float x, float z, float W, float H, float &sx, float &sy)
{
    float dx = (x - s.cx) * s.scale, dy = (z - s.cz) * s.scale, rx = dx, ry = dy;
    switch (s.zflag) {
        case 2: rx = dy;  ry = -dx; break;    // RIGHT
        case 3: rx = -dy; ry = -dx; break;    // DOWN(照抄游戏)
        case 4: rx = -dy; ry = dx;  break;    // LEFT
        default: break;                       // UP
    }
    const float cx = s.root_x + 0.5f * s.root_w, cy = s.root_y + 0.5f * s.root_h;
    const float ux = cx + 0.95f * rx, uy = cy + 0.95f * (20.0f + ry);
    const float k = W / s.dw;
    sx = ux * k;
    sy = H - uy * k;
}

} // namespace PyMadEye
