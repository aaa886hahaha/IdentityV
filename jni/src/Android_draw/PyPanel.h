#ifndef IDV_PY_PANEL_H
#define IDV_PY_PANEL_H
// ============================================================================
// 板子状态走 Python 层(2026-11-18，先验实现 + 对照日志，等上机日志纠正)
// ============================================================================
// 先验(全部来自静态源，没有活体验证过)：
//   units_by_type[8] = PanelUnit                 —— python-layer.md 2026-09-22 注入实测，一局 32 个
//   PanelUnit.state  = ComuKeys.PANEL_STATE      —— std_pyc ClientUnits/PanelUnit：__init__ 里 self.state = IDLE 直接赋值，
//                                                   set_state/recover_state 也是直接写 self.state ⇒ 普通实例属性，按名字可读
//     1 IDLE 立着 / 2 USEING 正在放(放倒动画中) / 3 USED 已放下 / 4 SPAN(含义未确认，set_state 里跟翻越有关) / 5 DESTROY 被砸
//   配对：unit.model + 0x20 = 场景对象(跟密码机/本体同一条)。但 PanelUnit 有三套静态模型
//         static_state_models{idle,putdown,destroy}，show_state_model 会切换显示 —— 数组里那个 woodplane 对象
//         不一定是 unit.model，所以指针配不上时再按坐标配(水平 1.5 单位内)，日志写明是哪种配上的。
//
// 只在 AutoPallet 线程里调用(单线程，表不加锁)。刷新 = 整表重扫，几十个单位 × 十来次驱动读，1 秒一次。
// 每轮读状态 = 实例视图 + 名字核对 + 读值(带指针重读/refcount/类型/值域 1~5 闸)，跟场景偏移那两跳同量级。
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include "PyCore.h"

namespace PyPanel {

static const int PANEL_UNIT_TYPE = 8;
static const int MAX_PANELS = 96;
static const int ST_IDLE = 1, ST_USEING = 2, ST_USED = 3, ST_SPAN = 4, ST_DESTROY = 5;

// 2026-11-18 第一次上机(用户本机 0917)：32 个全读出，但 model+0x20 只在放倒后配上 1 块、坐标配上 0 块。
// 状态值已对上：放板后 2(正在放)→3(已放下)→4(人翻越时)，同时场景值 16→196624(0x30010)。
// 所以加第三种配对：static_state_models{idle,putdown,destroy} 各自 +0x20 的场景对象；并打样本日志查坐标为什么对不上
static const char *const STATIC_KEYS[3] = {"idle", "putdown", "destroy"};
struct Panel { uint64_t inst, scene, st_scene[3]; float pos[3]; bool pos_ok, model_ok, ssm_ok; };
static Panel   g_list[MAX_PANELS];
static int     g_n = 0;
static int64_t g_i_unit_mgr = -1, g_i_ubt = -1, g_i_state = -1, g_i_model = -1, g_i_pos = -1, g_i_ssm = -1;
static char    g_status[128] = "";

static inline const char *state_name(int s)
{
    switch (s) {
    case ST_IDLE: return "立着"; case ST_USEING: return "正在放"; case ST_USED: return "已放下";
    case ST_SPAN: return "SPAN"; case ST_DESTROY: return "被砸"; default: return "?";
    }
}

// 整表重扫。返回读到的 PanelUnit 个数(0 = 不在局内/读不到，原因在 g_status)
static int refresh()
{
    g_n = 0;
    if (!PyRoot::g_ready.load()) { snprintf(g_status, sizeof(g_status), "Py根未就绪"); return 0; }
    const char *why = "";
    uint64_t md = PyCore::game_kernel_dict(why);
    if (!md) { snprintf(g_status, sizeof(g_status), "%s", why); return 0; }
    g_i_unit_mgr = PyCore::find_key(md, "unit_mgr", g_i_unit_mgr);
    uint64_t um = g_i_unit_mgr >= 0 ? PyCore::get_attr(md, g_i_unit_mgr) : 0;
    if (!PyCore::is_obj(um)) { snprintf(g_status, sizeof(g_status), "unit_mgr 无效"); return 0; }
    uint64_t ud = getPtr64(um - 0x18);
    g_i_ubt = PyCore::find_key(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = g_i_ubt >= 0 ? PyCore::get_attr(ud, g_i_ubt) : 0;
    if (!PyCore::is_obj(ubt)) { snprintf(g_status, sizeof(g_status), "units_by_type 无效"); return 0; }
    uint64_t lst = PyCore::get_type_list(ubt, PANEL_UNIT_TYPE);
    int64_t n = 0; uint64_t items = 0;
    if (!PyCore::read_list(lst, n, items)) { snprintf(g_status, sizeof(g_status), "没有 units_by_type[8]"); return 0; }
    for (int64_t i = 0; i < n && g_n < MAX_PANELS; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        PyCore::InstView v;
        if (!PyCore::inst_view(inst, v)) continue;
        bool ok = false;
        uint64_t mo = PyCore::inst_attr(v, g_i_model, "model", ok);
        Panel &p = g_list[g_n];
        p.inst = inst;
        p.model_ok = ok;
        p.scene = ok ? PyCore::get_scene_obj(mo) : 0;
        uint64_t pv = PyCore::inst_attr(v, g_i_pos, "position", ok);   // vector：+0x10/+0x14/+0x18 = X/高度/Y
        p.pos_ok = ok && vm_readv(pv + 0x10, p.pos, 12) && std::isfinite(p.pos[0]) && std::isfinite(p.pos[2]);
        if (!p.pos_ok) p.pos[0] = p.pos[1] = p.pos[2] = 0.f;
        // static_state_models：dict(str -> model)，三个模型各取 +0x20 场景对象
        p.st_scene[0] = p.st_scene[1] = p.st_scene[2] = 0;
        uint64_t sd = PyCore::inst_attr(v, g_i_ssm, "static_state_models", ok);
        PyCore::Dict sk;
        p.ssm_ok = ok && PyCore::read_dict(sd, sk);
        for (int64_t j = 0; p.ssm_ok && j < sk.n_entries && j < 8; j++) {
            char kn[16] = {0};
            if (!PyCore::read_str(getPtr64(PyCore::key_slot(sk, j)), kn, sizeof(kn))) continue;
            for (int k = 0; k < 3; k++)
                if (strcmp(kn, STATIC_KEYS[k]) == 0) p.st_scene[k] = PyCore::get_scene_obj(getPtr64(PyCore::value_slot(sk, j)));
        }
        g_n++;
    }
    snprintf(g_status, sizeof(g_status), "units_by_type[8] %lld 个，读出 %d 个", (long long)n, g_n);
    return g_n;
}

// 场景板子 obj 对应表里第几个；how = 1 model 指针 / 2 坐标 / 3+k static_state_models[k] 指针；配不上返回 -1
// ⚠ pos 是 AutoPallet 的约定 {X, 水平Y, 高度}(read_pos 把水平两轴放前面)；g_list[].pos 是 Python 的 {X, 高度, Y}
// (11-18 第二次上机：第一版按同一顺序比，坐标配对 0/32；样本里 Python(-577.6,0.0,52.2) 对 场景板(-577.6,52.2,0.0) 其实是同一点)
// 同日确认：unit.model == static_state_models['idle']，+0x20 是 *_woodplane001_lod_up.gim；
// 而读线程收进来的场景板是 *_woodplane001.gim(另一个对象)，所以立着时指针配不上，只能靠坐标
static int find(uint64_t obj, const float *pos, int &how)
{
    how = 0;
    for (int i = 0; i < g_n; i++) if (g_list[i].scene && g_list[i].scene == obj) { how = 1; return i; }
    for (int i = 0; i < g_n; i++)
        for (int k = 0; k < 3; k++) if (g_list[i].st_scene[k] && g_list[i].st_scene[k] == obj) { how = 3 + k; return i; }
    int best = -1; float bd = 1.5f;
    for (int i = 0; i < g_n; i++) {
        if (!g_list[i].pos_ok) continue;
        float dx = g_list[i].pos[0] - pos[0], dy = g_list[i].pos[2] - pos[1];
        float d = sqrtf(dx * dx + dy * dy);
        if (d < bd && fabsf(g_list[i].pos[1] - pos[2]) < 10.f) { bd = d; best = i; }
    }
    if (best >= 0) how = 2;
    return best;
}

// 读第 i 块的 state(1~5)。读不到返回 false
static bool read_state(int i, int &st)
{
    if (i < 0 || i >= g_n) return false;
    PyCore::InstView v;
    if (!PyCore::inst_view(g_list[i].inst, v)) return false;
    bool ok = false;
    PyCore::inst_attr(v, g_i_state, "state", ok);   // 只为核对/更新序号缓存
    if (!ok || g_i_state < 0) return false;
    float f = 0.f;
    if (!PyCore::read_number(PyCore::inst_value_slot(v, g_i_state), f, 0.5, 5.5)) return false;
    st = (int)lroundf(f);
    return true;
}

static inline const char *status_text() { return g_status[0] ? g_status : "未刷新"; }
static inline const char *how_name(int how)
{
    switch (how) { case 1: return "model指针"; case 2: return "坐标"; case 3: return "idle模型"; case 4: return "putdown模型"; case 5: return "destroy模型"; default: return "无"; }
}

} // namespace PyPanel
#endif
