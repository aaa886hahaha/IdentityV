#ifndef IDV_PY_WATERFIGHT_H
#define IDV_PY_WATERFIGHT_H

// ============================================================================
// 林中童话（娱乐模式，底层 = 打水仗 WorldBattleWaterFight）—— 玩家 + 物资读取
// ============================================================================
//
// 2026-10-19 probe2 活体 16 样本 + 静态源(ClientUnits/WaterFightPlayerUnit.py 等)实测结论：
//   世界 = WorldBattleWaterFight(world_type=95)，大型多人 AOI('mid')：只同步你附近的实体，
//   所以能读到/能画的就只有 AOI 半径内的玩家/物资，远处没对象、读不出。硬限制，非 bug。
//
//   玩家 [1601] WaterFightPlayerUnit：
//     position            -> +0x10/+0x14/+0x18 = X/高度/Y(vector)。！！visual_position/interp_pos/
//                            logic_position 对玩家全是 None(平滑渲染位在 native/模型层，Python 读不到)——
//                            所以框抖只能靠提高刷新率(16ms)缓解，不能换字段。
//     water_fight_team    -> 1~8 真队伍(每队 2 人，"八个阵营两两组队"指的就是它)。队友 = 同 team。
//     water_fight_identity-> 1~10 动物(个人，不是队：同队两人动物可不同)：1马鹿2野猫3狐狸4獾5雪兔
//                            6金雕7松鼠8草蛇9蝙蝠10野猪。只做 label，不做配色。
//     is_self             -> 本机(跳过自己 + 取自身 team/坐标当参照)
//     is_leave            -> 0=在场；非 0=离场/出局(静态 handle_leave_game→LEAVE_TYPE.DEAD)。过滤死人用它。
//
//   物资：
//     [1602] WaterFightBoxUnit  box_type + level。box_type==3 恒 80 个 = 地面/草装饰(静态不随对局变)，
//            box_type 0/2 随对局消耗变化 = 真补给箱 -> 只收 box_type != 3。
//     [1606] WaterFightDropItemUnit 掉落(真实拾取物) -> 全收。
//     物资静态不动，刷新降到 ~500ms(省 CPU)；玩家 16ms(抗抖)。两者分开双缓冲、分开刷新率。
//
// 结构/双缓冲/PyRoot/PyCore 用法照 PyProgress.h；三个坑(逐实例算 entries/值类型/防竞态)同样适用。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <thread>
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"
#include "PyCore.h"

namespace PyWaterFight {

static const int PLAYER_UNIT_TYPE = 1601;
static const int BOX_UNIT_TYPE    = 1602;
static const int DROP_UNIT_TYPE   = 1606;
static const int BOX_CLUTTER_TYPE = 3;      // box_type==3 恒 80 个 = 地面/草，过滤掉

static const int MAX_PLAYERS = 32;
static const int MAX_LOOT    = 400;

struct Player { float x, h, z; int team, identity; bool is_self; };
struct Loot   { float x, h, z; };

// 玩家、物资各自一套双缓冲(独立刷新率)
static Player g_players[2][MAX_PLAYERS];
static volatile int g_pcount[2] = {0, 0};
static std::atomic<int> g_pactive{0};
static Loot   g_loot[2][MAX_LOOT];
static volatile int g_lcount[2] = {0, 0};
static std::atomic<int> g_lactive{0};

static uintptr_t g_libbase = 0;
static uint64_t  g_module_dict = 0;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_pos = -1, g_i_team = -1, g_i_identity = -1, g_i_self = -1;
static int64_t   g_i_box_pos = -1, g_i_box_type = -1, g_i_drop_pos = -1;
static volatile bool g_available = false;      // 玩家读取状态(主)
static volatile bool g_loot_avail = false;
static volatile int  g_fail_streak = 0;
static char g_status[96] = "";   // 空 = 未启动，见 status_text()

using PyCore::is_obj; using PyCore::Dict; using PyCore::read_dict;
using PyCore::value_slot; using PyCore::find_key; using PyCore::get_attr;
using PyCore::resolve_index; using PyCore::get_type_list; using PyCore::read_bool; using PyCore::read_int;

// position：取指针→读 12 字节→重读指针确认没变(防竞态)
static bool read_pos(uint64_t slot, float out[3])
{
    uint64_t pp = getPtr64(slot);
    if (!is_obj(pp)) return false;
    if (!vm_readv(pp + 0x10, out, 12)) return false;
    if (getPtr64(slot) != pp) return false;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

static bool resolve_module()
{
    const char *why = "";
    g_module_dict = PyCore::game_kernel_dict(why);
    if (!g_module_dict) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }
    return true;
}

// 定位 units_by_type。玩家/物资两条刷新都从这里拿 ubt
static uint64_t get_ubt()
{
    if (g_module_dict == 0 && !resolve_module()) return 0;
    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? get_attr(g_module_dict, g_i_unit_mgr) : 0;
    if (!is_obj(um)) { snprintf(g_status, sizeof(g_status), "%s", T("unit_mgr 无效(未在对局中?)", "unit_mgr invalid (not in match?)")); return 0; }
    uint64_t ud = getPtr64(um - 0x18);
    g_i_ubt = find_key(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = (g_i_ubt >= 0) ? get_attr(ud, g_i_ubt) : 0;
    if (!is_obj(ubt)) { snprintf(g_status, sizeof(g_status), "%s", T("units_by_type 无效", "units_by_type invalid")); return 0; }
    return ubt;
}

static bool refresh_players(uint64_t ubt)
{
    uint64_t lst = get_type_list(ubt, PLAYER_UNIT_TYPE);
    if (!is_obj(lst)) { snprintf(g_status, sizeof(g_status), "%s", T("不是林中童话局(无[1601])", "not an Enchanted Forest match (no [1601])")); return false; }
    int64_t n = 0;
    vm_readv(lst + 0x10, &n, 8);
    uint64_t items = getPtr64(lst + 0x18);
    if (n <= 0 || n > 64 || !is_obj(items)) { snprintf(g_status, sizeof(g_status), "%s", T("玩家列表异常", "player list malformed")); return false; }
    if (n > MAX_PLAYERS) n = MAX_PLAYERS;

    int write_idx = 1 - g_pactive;
    int pc = 0;
    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!is_obj(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);
        Dict dk;
        if (!read_dict(d, dk)) continue;

        g_i_pos      = resolve_index(d, dk, g_i_pos,      "position");
        g_i_team     = resolve_index(d, dk, g_i_team,     "water_fight_team");
        g_i_identity = resolve_index(d, dk, g_i_identity, "water_fight_identity");
        g_i_self     = resolve_index(d, dk, g_i_self,     "is_self");
        if (g_i_pos < 0) continue;

        float xyz[3];
        if (!read_pos(value_slot(dk, g_i_pos), xyz)) continue;

        int64_t tv = 0, iv = 0;
        int team = 0, identity = 0;
        if (g_i_team >= 0 && read_int(getPtr64(value_slot(dk, g_i_team)), tv) && tv >= 1 && tv <= 8) team = (int)tv;
        if (g_i_identity >= 0 && read_int(getPtr64(value_slot(dk, g_i_identity)), iv) && iv >= 1 && iv <= 10) identity = (int)iv;
        bool self = false;
        if (g_i_self >= 0) read_bool(value_slot(dk, g_i_self), self);

        g_players[write_idx][pc].x = xyz[0];
        g_players[write_idx][pc].h = xyz[1];
        g_players[write_idx][pc].z = xyz[2];
        g_players[write_idx][pc].team = team;
        g_players[write_idx][pc].identity = identity;
        g_players[write_idx][pc].is_self = self;
        pc++;
    }
    g_pcount[write_idx] = pc;
    g_pactive = write_idx;
    snprintf(g_status, sizeof(g_status), T("正常 玩家%d 物资%d", "OK players %d loot %d"), pc, g_loot_avail ? g_lcount[g_lactive] : 0);
    return true;
}

static void refresh_loot(uint64_t ubt)
{
    int write_idx = 1 - g_lactive;
    int lc = 0;
    // 箱子 [1602]：只收 box_type != 3
    uint64_t lst = get_type_list(ubt, BOX_UNIT_TYPE);
    if (is_obj(lst)) {
        int64_t n = 0; vm_readv(lst + 0x10, &n, 8);
        uint64_t items = getPtr64(lst + 0x18);
        if (n > 0 && n <= 1024 && is_obj(items)) {
            for (int64_t i = 0; i < n && lc < MAX_LOOT; i++) {
                uint64_t inst = getPtr64(items + i * 8);
                if (!is_obj(inst)) continue;
                uint64_t d = getPtr64(inst - 0x18);
                Dict dk;
                if (!read_dict(d, dk)) continue;
                g_i_box_pos  = resolve_index(d, dk, g_i_box_pos,  "position");
                g_i_box_type = resolve_index(d, dk, g_i_box_type, "box_type");
                if (g_i_box_pos < 0) continue;
                int64_t bt = 0;
                if (g_i_box_type >= 0 && read_int(getPtr64(value_slot(dk, g_i_box_type)), bt) && bt == BOX_CLUTTER_TYPE)
                    continue;                                  // 地面/草，跳过
                float xyz[3];
                if (!read_pos(value_slot(dk, g_i_box_pos), xyz)) continue;
                g_loot[write_idx][lc].x = xyz[0]; g_loot[write_idx][lc].h = xyz[1]; g_loot[write_idx][lc].z = xyz[2]; lc++;
            }
        }
    }
    // 掉落 [1606]：全收
    lst = get_type_list(ubt, DROP_UNIT_TYPE);
    if (is_obj(lst)) {
        int64_t n = 0; vm_readv(lst + 0x10, &n, 8);
        uint64_t items = getPtr64(lst + 0x18);
        if (n > 0 && n <= 1024 && is_obj(items)) {
            for (int64_t i = 0; i < n && lc < MAX_LOOT; i++) {
                uint64_t inst = getPtr64(items + i * 8);
                if (!is_obj(inst)) continue;
                uint64_t d = getPtr64(inst - 0x18);
                Dict dk;
                if (!read_dict(d, dk)) continue;
                g_i_drop_pos = resolve_index(d, dk, g_i_drop_pos, "position");
                if (g_i_drop_pos < 0) continue;
                float xyz[3];
                if (!read_pos(value_slot(dk, g_i_drop_pos), xyz)) continue;
                g_loot[write_idx][lc].x = xyz[0]; g_loot[write_idx][lc].h = xyz[1]; g_loot[write_idx][lc].z = xyz[2]; lc++;
            }
        }
    }
    g_lcount[write_idx] = lc;
    g_lactive = write_idx;
    g_loot_avail = true;
}

static const int PLAYER_INTERVAL_MS = 16;    // 玩家每轮刷(抗框抖)
static const int LOOT_EVERY_CYCLES  = 30;    // 物资每 ~480ms 刷一次(静态不动，省 CPU)

static void thread_main()
{
    int cycle = 0;
    while (true) {
        if (g_libbase && PyRoot::ensure()) {
            uint64_t ubt = get_ubt();
            if (ubt) {
                bool ok = refresh_players(ubt);
                if (cycle % LOOT_EVERY_CYCLES == 0) refresh_loot(ubt);
                if (ok) { g_available = true; g_fail_streak = 0; }
                else    { g_available = false; g_loot_avail = false; if (++g_fail_streak >= 16) { g_module_dict = 0; g_i_unit_mgr = g_i_ubt = -1; g_fail_streak = 0; } }
            } else { g_available = false; g_loot_avail = false; }
        } else {
            g_available = false;
            if (g_libbase) snprintf(g_status, sizeof(g_status), "%s", PyRoot::status_text());
        }
        cycle++;
        usleep(PLAYER_INTERVAL_MS * 1000);
    }
}

static void start(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase = libbase;
    snprintf(g_status, sizeof(g_status), "%s", T("启动中", "starting"));
    std::thread(thread_main).detach();
    printf("[林中童话] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------
static inline bool available() { return g_available; }
static inline const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

static inline int player_count() { return g_available ? g_pcount[g_pactive] : 0; }
static bool get_player(int i, Player &out)
{
    if (!g_available) return false;
    int b = g_pactive;
    if (i < 0 || i >= g_pcount[b]) return false;
    out = g_players[b][i];
    return true;
}

// 自身坐标 + 队伍(WaterFight 玩家没经典锚点，距离参照和队友判定都靠 is_self 这一项)
static bool self_ref(float &x, float &h, float &z, int &team)
{
    if (!g_available) return false;
    int b = g_pactive, n = g_pcount[b];
    for (int i = 0; i < n; i++)
        if (g_players[b][i].is_self) { x = g_players[b][i].x; h = g_players[b][i].h; z = g_players[b][i].z; team = g_players[b][i].team; return true; }
    return false;
}

static inline int loot_count() { return g_loot_avail ? g_lcount[g_lactive] : 0; }
static bool get_loot(int i, Loot &out)
{
    if (!g_loot_avail) return false;
    int b = g_lactive;
    if (i < 0 || i >= g_lcount[b]) return false;
    out = g_loot[b][i];
    return true;
}

} // namespace PyWaterFight

#endif // IDV_PY_WATERFIGHT_H
