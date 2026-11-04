#ifndef IDV_PY_ITEMS_H
#define IDV_PY_ITEMS_H

// ============================================================================
// 道具白名单 —— 游戏自己的道具清单 cur_world.item_mgr._uid2item(2026-10-30 静态反编译，未上机)
// ============================================================================
//
// 为什么要它：场景数组里同类名的 h55_pendant_* 对象比真实道具多。道具用完后 Python 侧已经销毁，
// 场景数组里却还留着模型，叠加层只认类名就照画(9-22 实测一局 3 个飞镖只有 1 个有宿主)。
//
// 用完的流程(common_cs/items/ItemBase.use_out、ItemMgr、worlds/BattleEvent.handle_item_event)：
//   服务端 use_out -> ItemMgr.delete_item -> 下发 ITEM_OPT.DELETE -> 客户端 delete_item(uid) 进 to_delete_uids
//   -> 下一次 WorldBattleBase.game_tick 调 ItemMgr.tick，从 _uid2item 弹出并 item.destroy() -> model.destroy()
//   所以"还在 _uid2item 里" = 道具还活着。手上的(owner=单位)、地上的(owner=None)都在里面。
//   例外：FILL_CNT/UNDERTAKER 这类可充能道具用完不删，本来就还在玩家手上，照画没错。
//
// 每件道具收这些(都是 ItemBase 实例字典里的普通属性，没有 __slots__)：
//   model / another_model  -> +0x20 场景对象，绘制循环按它配对
//   owner                  -> None 或单位；单位时再读 owner.uid 和 owner.model+0x20
//   model_bind_type        -> 非 0 = 不挂模型、往主人身上 add_mesh(只有护肘 63/2004)。
//                             这时 item.model 不可见、停在创建点不动，场景里那个就是位置错的护肘
// 玩具商扔出去的包(units_by_type[451] ToyPackageUnit)：包里的道具 owner=None，包贴到某个求生者身上时
//   客户端每 tick 把道具模型挪到那个人脚下(iCommonToyPackage.update_attached_package_pos)。
//   收 package_item_uid -> auto_attach_uid，绘制侧用来跳过"贴在我身上的包"。
// 哭泣小丑骑的火箭(units_by_type[511] SpJokerRocketUnit)是单位不是道具，但模型跟手持火箭同一个
//   (chr/prop/h55_pendant_huojian，ClientUnits/SpJokerRocketUnit.__init__ 默认 model)，不收的话会被白名单挡掉。
//   当成"主人 = owner_uid"的条目收进来：照画，自己的那支不画。爆炸/耗尽时单位销毁，自然出清单。
//
// 读不全(某件道具的实例字典/属性读失败)时 complete=false：绘制侧对查不到的对象照旧画，
// 宁可多画一个残留模型，也不能因为一次读失败把真道具藏掉。
// 只在「绘制道具」开着时读(set_enabled)，关着不碰游戏内存。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"
#include "PyCore.h"

namespace PyItems {

static const int MAX_ITEMS = 96;
static const int TOY_PACKAGE_TYPE = 451;      // ComuKeys.UNIT_TYPE_CONF.TOY_PACKAGE
static const int MAX_PACKAGES = 16;
static const int SP_JOKER_ROCKET_TYPE = 511;  // ComuKeys.UNIT_TYPE_CONF.SP_JOKER_ROCKET

struct Entry {
    uint64_t model;        // item.model+0x20，0 = 没有/读不到
    uint64_t another;      // item.another_model+0x20
    uint64_t owner_body;   // owner.model+0x20，0 = 没主人或读不到
    int64_t  owner_uid;    // -1 = 没主人
    int64_t  attach_uid;   // 所在的包贴在谁身上，-1 = 不在包里/没贴人
    int      bind_type;    // model_bind_type
    bool     owned;        // owner 不是 None
};

struct Snapshot {
    Entry   e[MAX_ITEMS];
    int     n;
    bool    complete;      // 每件都读全了，见文件头
    int64_t main_uid;      // GK.g_unit.uid(我的主角色，机械师操控玩偶时 cam.unit 是玩偶)，-1 = 读不到
};

static Snapshot g_buf[2];
static std::atomic<int> g_active{0};
static std::atomic<bool> g_available{false};
static std::atomic<bool> g_enabled{false};
static inline void set_enabled(bool on) { g_enabled = on; }

static uintptr_t g_libbase = 0;
static uint64_t  g_module_dict = 0;
static uint64_t  g_none = 0;                  // None 单例，第一次按类型名认出来后缓存
static int64_t   g_i_world_mgr = -1, g_i_cur_world = -1, g_i_item_mgr = -1, g_i_uid2item = -1;
static int64_t   g_i_gunit = -1, g_i_gunit_uid = -1;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_pkg_item = -1, g_i_pkg_attach = -1;
static int64_t   g_i_rk_model = -1, g_i_rk_owner = -1;
static int       g_fail_streak = 0;
static char      g_status[96] = "";

using PyCore::is_obj; using PyCore::Dict; using PyCore::read_dict;
using PyCore::key_slot; using PyCore::value_slot; using PyCore::find_key; using PyCore::get_attr;
using PyCore::read_int; using PyCore::get_scene_obj;
using PyCore::read_list; using PyCore::get_type_list;
using PyCore::InstView; using PyCore::inst_view; using PyCore::inst_attr;
// 实例属性一律走 InstView(PyCore.h)：ItemMgr、道具实例属性少，多半没有物化的 dict，get_inst_dict 读不到

// 序号缓存按类分开存(键 = 实例 ob_type)：道具子类上百个、字典形状各不同，共用一个缓存会每件都重扫。
// 表满了退回共用的那组，resolve_index 每次按名字核对，不会读错属性
enum { A_MODEL, A_ANOTHER, A_OWNER, A_BIND, A_COUNT };
static const char *const ITEM_ATTRS[A_COUNT] = { "model", "another_model", "owner", "model_bind_type" };
enum { O_MODEL, O_UID, O_COUNT };
static const char *const OWNER_ATTRS[O_COUNT] = { "model", "uid" };
struct ClassCache { uint64_t type; int64_t idx[A_COUNT]; };
static const int MAX_CLASSES = 32;
static ClassCache g_item_cls[MAX_CLASSES], g_owner_cls[MAX_CLASSES];
static int g_item_cls_n = 0, g_owner_cls_n = 0;
static int64_t g_item_shared[A_COUNT] = {-1, -1, -1, -1}, g_owner_shared[A_COUNT] = {-1, -1, -1, -1};

static int64_t *class_slots(ClassCache *tbl, int &cnt, int64_t *shared, uint64_t type)
{
    for (int i = 0; i < cnt; i++) if (tbl[i].type == type) return tbl[i].idx;
    if (cnt >= MAX_CLASSES) return shared;
    tbl[cnt].type = type;
    for (int a = 0; a < A_COUNT; a++) tbl[cnt].idx[a] = -1;
    return tbl[cnt++].idx;
}

static bool is_none(uint64_t v)
{
    if (!is_obj(v)) return false;
    if (g_none) return v == g_none;
    if (PyRoot::type_name_is(getPtr64(v + 8), "NoneType")) { g_none = v; return true; }
    return false;
}

// 取实例的某个属性对象，再给它建视图。哪一步不行都返回 false
static bool attr_view(const InstView &v, int64_t &idx, const char *name, InstView &out)
{
    bool ok;
    uint64_t o = inst_attr(v, idx, name, ok);
    return ok && inst_view(o, out);
}

// int 或 None。None -> out=-1；都不是 -> false
static bool read_int_or_none(uint64_t v, int64_t &out)
{
    if (is_none(v)) { out = -1; return true; }
    return read_int(v, out);
}

static bool resolve_module()
{
    const char *why = "";
    g_module_dict = PyCore::game_kernel_dict(why);
    if (!g_module_dict) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }
    return true;
}

static uint64_t get_ubt()
{
    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    InstView uv;
    if (!inst_view(g_i_unit_mgr >= 0 ? get_attr(g_module_dict, g_i_unit_mgr) : 0, uv)) return 0;
    bool ok;
    uint64_t ubt = inst_attr(uv, g_i_ubt, "units_by_type", ok);
    return ok && is_obj(ubt) ? ubt : 0;
}

// 哭泣小丑骑的火箭，见文件头。没有 511 键(没有小丑/没开过火箭)时 read_list 失败，什么都不收
static void collect_rockets(uint64_t ubt, Snapshot &res)
{
    uint64_t lst = ubt ? get_type_list(ubt, SP_JOKER_ROCKET_TYPE) : 0;
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lst, n, items)) return;
    for (int64_t i = 0; i < n; i++) {
        if (res.n >= MAX_ITEMS) { res.complete = false; return; }
        InstView rv;
        if (!inst_view(getPtr64(items + i * 8), rv)) { res.complete = false; continue; }
        bool ok1, ok2;
        uint64_t mv = inst_attr(rv, g_i_rk_model, "model", ok1);
        uint64_t ov = inst_attr(rv, g_i_rk_owner, "owner_uid", ok2);
        if (!ok1 || is_none(mv)) { res.complete = false; continue; }
        Entry &e = res.e[res.n];
        memset(&e, 0, sizeof(e));
        e.model = get_scene_obj(mv);
        e.owned = true;                                   // 主人 = 召出它的小丑，自己的那支不画
        e.owner_uid = -1;
        if (ok2) read_int(ov, e.owner_uid);
        e.attach_uid = -1;
        res.n++;
    }
}

// 包 -> 贴在谁身上。读不到返回 0 条，不影响道具清单本身
static int collect_packages(uint64_t ubt, int64_t *item_uid, int64_t *attach_uid, int cap)
{
    uint64_t lst = ubt ? get_type_list(ubt, TOY_PACKAGE_TYPE) : 0;
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lst, n, items)) return 0;
    int cnt = 0;
    for (int64_t i = 0; i < n && cnt < cap; i++) {
        InstView pv;
        if (!inst_view(getPtr64(items + i * 8), pv)) continue;
        bool ok1, ok2;
        uint64_t vi = inst_attr(pv, g_i_pkg_item, "package_item_uid", ok1);
        uint64_t va = inst_attr(pv, g_i_pkg_attach, "auto_attach_uid", ok2);
        int64_t iu = -1, au = -1;
        if (!ok1 || !ok2 || !read_int(vi, iu) || !read_int_or_none(va, au) || au < 0) continue;
        item_uid[cnt] = iu; attach_uid[cnt] = au; cnt++;
    }
    return cnt;
}

static bool try_refresh(Snapshot &res)
{
    if (g_module_dict == 0 && !resolve_module()) return false;

    // GK.g_world_mgr.cur_world.item_mgr._uid2item
    g_i_world_mgr = find_key(g_module_dict, "g_world_mgr", g_i_world_mgr);
    InstView wv, cv, iv;
    if (!inst_view(g_i_world_mgr >= 0 ? get_attr(g_module_dict, g_i_world_mgr) : 0, wv)) {
        snprintf(g_status, sizeof(g_status), "%s", T("g_world_mgr 无效", "g_world_mgr invalid")); return false;
    }
    if (!attr_view(wv, g_i_cur_world, "cur_world", cv)) {
        snprintf(g_status, sizeof(g_status), "%s", T("cur_world 无效", "cur_world invalid")); return false;
    }
    if (!attr_view(cv, g_i_item_mgr, "item_mgr", iv)) {
        snprintf(g_status, sizeof(g_status), "%s", T("没有 item_mgr(不在对局中?)", "no item_mgr (not in match?)")); return false;
    }
    bool u2i_ok;
    uint64_t u2i = inst_attr(iv, g_i_uid2item, "_uid2item", u2i_ok);
    Dict uk;
    if (!u2i_ok || !is_obj(u2i) || getPtr64(u2i + 8) != PyRoot::g_dict_type || !read_dict(u2i, uk)) {
        snprintf(g_status, sizeof(g_status), "%s", T("_uid2item 读不到", "_uid2item unreadable")); return false;
    }

    // 我的主角色 uid(GK.g_unit)
    res.main_uid = -1;
    g_i_gunit = find_key(g_module_dict, "g_unit", g_i_gunit);
    InstView gv;
    if (inst_view(g_i_gunit >= 0 ? get_attr(g_module_dict, g_i_gunit) : 0, gv)) {
        bool ok; uint64_t v = inst_attr(gv, g_i_gunit_uid, "uid", ok);
        int64_t u = -1;
        if (ok && read_int(v, u)) res.main_uid = u;
    }

    const uint64_t ubt = get_ubt();
    int64_t pk_item[MAX_PACKAGES], pk_attach[MAX_PACKAGES];
    int pk_n = collect_packages(ubt, pk_item, pk_attach, MAX_PACKAGES);

    res.n = 0;
    res.complete = true;
    for (int64_t i = 0; i < uk.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(uk, i));
        if (kp == 0) continue;                                   // 已删除的条目
        uint64_t inst = getPtr64(value_slot(uk, i));
        if (inst == 0) continue;
        if (res.n >= MAX_ITEMS) { res.complete = false; break; }
        Entry &e = res.e[res.n];
        memset(&e, 0, sizeof(e));
        e.owner_uid = e.attach_uid = -1;

        int64_t uid = -1;
        InstView dv;
        if (!read_int(kp, uid) || !inst_view(inst, dv)) { res.complete = false; continue; }
        int64_t *ix = class_slots(g_item_cls, g_item_cls_n, g_item_shared, getPtr64(inst + 8));

        bool ok;
        uint64_t mv = inst_attr(dv, ix[A_MODEL], ITEM_ATTRS[A_MODEL], ok);
        if (!ok) { res.complete = false; continue; }
        if (!is_none(mv)) e.model = get_scene_obj(mv);
        uint64_t av = inst_attr(dv, ix[A_ANOTHER], ITEM_ATTRS[A_ANOTHER], ok);
        if (ok && !is_none(av)) e.another = get_scene_obj(av);
        uint64_t bv = inst_attr(dv, ix[A_BIND], ITEM_ATTRS[A_BIND], ok);
        int64_t bt = 0;
        if (ok && read_int(bv, bt)) e.bind_type = (int)bt;

        uint64_t ov = inst_attr(dv, ix[A_OWNER], ITEM_ATTRS[A_OWNER], ok);
        if (!ok) { res.complete = false; continue; }
        if (!is_none(ov)) {
            InstView ow;
            if (!inst_view(ov, ow)) { res.complete = false; continue; }   // 有主人但读不出是谁：这件不收
            e.owned = true;
            int64_t *ox = class_slots(g_owner_cls, g_owner_cls_n, g_owner_shared, getPtr64(ov + 8));
            uint64_t omv = inst_attr(ow, ox[O_MODEL], OWNER_ATTRS[O_MODEL], ok);
            if (ok && !is_none(omv)) e.owner_body = get_scene_obj(omv);
            uint64_t ouv = inst_attr(ow, ox[O_UID], OWNER_ATTRS[O_UID], ok);
            int64_t ou = -1;
            if (ok && read_int(ouv, ou)) e.owner_uid = ou;
        }
        for (int p = 0; p < pk_n; p++) if (pk_item[p] == uid) { e.attach_uid = pk_attach[p]; break; }
        res.n++;
    }
    collect_rockets(ubt, res);
    // 共享键偏移 0 = 这一路全是物化的 dict，没走到 values 读法
    snprintf(g_status, sizeof(g_status), T("正常 道具%d%s 共享键+0x%x", "OK items %d%s cached_keys+0x%x"), res.n,
             res.complete ? "" : T(" (读不全)", " (partial)"), PyCore::g_cached_keys_off.load());
    return true;
}

static const int REFRESH_INTERVAL_MS = 100;

static void thread_main()
{
    static Snapshot work;
    bool was_ok = false;
    static char logged[96] = "";
    while (true) {
        if (!g_enabled.load()) {
            g_available = false;
            snprintf(g_status, sizeof(g_status), "%s", T("未开启", "off"));
            usleep(REFRESH_INTERVAL_MS * 1000);
            continue;
        }
        bool ok = false;
        if (g_libbase && PyRoot::ensure()) {
            ok = try_refresh(work);
            if (ok) {
                int w = 1 - g_active.load();
                g_buf[w] = work;
                g_active.store(w);
                g_fail_streak = 0;
            } else if (++g_fail_streak >= 8) {           // 连续失败：序号缓存作废，下一轮按名字重找
                g_module_dict = 0;
                g_i_world_mgr = g_i_cur_world = g_i_item_mgr = g_i_uid2item = -1;
                g_i_gunit = g_i_gunit_uid = g_i_unit_mgr = g_i_ubt = g_i_pkg_item = g_i_pkg_attach = -1;
                g_i_rk_model = g_i_rk_owner = -1;
                g_item_cls_n = g_owner_cls_n = 0;
                for (int a = 0; a < A_COUNT; a++) g_item_shared[a] = g_owner_shared[a] = -1;
                g_fail_streak = 0;
            }
        } else if (g_libbase) {
            snprintf(g_status, sizeof(g_status), "%s", PyRoot::status_text());
        }
        g_available = ok;
        // 变成可用/不可用时打一行；不可用期间原因变了也打(原来只在切换时打，一直失败就一行都没有，看不出卡在哪步)
        if (ok != was_ok || (!ok && strcmp(logged, g_status) != 0)) {
            printf("[道具] %s\n", g_status); fflush(stdout);
            was_ok = ok;
            snprintf(logged, sizeof(logged), "%s", g_status);
        }
        usleep(REFRESH_INTERVAL_MS * 1000);
    }
}

static void start(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase = libbase;
    snprintf(g_status, sizeof(g_status), "%s", T("启动中", "starting"));
    std::thread(thread_main).detach();
    printf("[道具] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------
static inline bool available() { return g_available.load(); }
static inline const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }
static inline bool complete() { return g_available.load() && g_buf[g_active.load()].complete; }
static inline int64_t main_uid() { return g_available.load() ? g_buf[g_active.load()].main_uid : -1; }
static inline int count() { return g_available.load() ? g_buf[g_active.load()].n : 0; }

static bool get(int i, Entry &out)
{
    if (!g_available.load()) return false;
    const Snapshot &s = g_buf[g_active.load()];
    if (i < 0 || i >= s.n) return false;
    out = s.e[i];
    return true;
}

// 场景对象 -> 道具。查不到返回 false(= 不是活着的道具，或这轮没读全，看 complete())
static bool lookup(uint64_t obj, Entry &out)
{
    if (!g_available.load() || obj == 0) return false;
    const Snapshot &s = g_buf[g_active.load()];
    for (int i = 0; i < s.n; i++)
        if (s.e[i].model == obj || s.e[i].another == obj) { out = s.e[i]; return true; }
    return false;
}

} // namespace PyItems

#endif // IDV_PY_ITEMS_H
