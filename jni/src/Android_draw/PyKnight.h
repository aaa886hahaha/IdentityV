#ifndef IDV_PY_KNIGHT_H
#define IDV_PY_KNIGHT_H

// ============================================================================
// 骑士预测 —— 我是监管时显示骑士对我的预测内容(技能/普攻/相持)
// 静态依据：11-13 热更 common_cs/mgr/ProphetMgr.py(CProphetMgr)、worlds/BattleEvent.handle_prophet_event，
// 记录在 逆向\第五人格_脚本解包_反作弊\SKILL.md §7.11。2026-11-15 写，未上机。
// ============================================================================
//
// 链路：GK.g_world_mgr.cur_world(读不到再试 battle_world).prophet_mgr.behavior_info
//   behavior_info = dict{骑士uid(int): list[pred]}，服务端 sync_prophet_mgr 下发、客户端 on_event 原样 update 进来，
//   BattleEvent.handle_prophet_event 不按身份过滤。监管自己的界面只拿它画问号(ico_question_1/2)。
// pred = 普通 dict，int 键(const_prophet.PK)：
//   0 ACTION   预测内容。>>8 分组：2=普攻(512) 3=技能(768~775) 4=相持(1024)
//   2 PS       2/3=预测中  0=对 1=错(客户端保留约 2 秒后清掉)
//   5 TARGET   list[监管uid]
//   7 USE_ID / 9 USE_SUB_ID   同一次使用的第一/二段
// ⚠ 唯一没坐实的：服务端发给监管端的数据里 ACTION 有没有被删掉(sync_battle_event 是服务端代码，客户端包里没有)。
//   所以第一次读到预测时打一行 [骑士] 日志(含 ACTION 原值)，上机后看日志就能判断。
// 只在开关打开、局内、我是监管时读(set_enabled)，关着不碰游戏内存。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"
#include "PyCore.h"

namespace PyKnight {

static const int MAX_PREDS = 8;
static const int MAX_TARGETS = 4;

enum { K_ACTION = 0, K_PS = 2, K_TARGET = 5, K_USE_ID = 7, K_USE_SUB_ID = 9 };

struct Pred {
    int64_t knight_uid;
    int     action;        // ACTION 原值，-1 = 没有这个键
    int     sub;           // USE_SUB_ID，读不到 = 0
    int64_t use_id;        // USE_ID，读不到 = -1
    int64_t tgt[MAX_TARGETS];
    int     tn;
};

struct Snapshot {
    Pred    p[MAX_PREDS];  // 只收"预测中"的
    int     n;
    int64_t main_uid;      // GK.g_unit.uid，-1 = 读不到
};

static Snapshot g_buf[2];
static std::atomic<int> g_active{0};
static std::atomic<bool> g_available{false};
static std::atomic<bool> g_enabled{false};
static inline void set_enabled(bool on) { g_enabled = on; }

static uintptr_t g_libbase = 0;
static uint64_t  g_module_dict = 0;
static int64_t   g_i_world_mgr = -1, g_i_cur_world = -1, g_i_battle_world = -1, g_i_prophet = -1, g_i_binfo = -1;
static int64_t   g_i_gunit = -1, g_i_gunit_uid = -1;
static int       g_fail_streak = 0;
static char      g_status[96] = "";

using PyCore::is_obj; using PyCore::Dict; using PyCore::read_dict;
using PyCore::key_slot; using PyCore::value_slot; using PyCore::find_key; using PyCore::get_attr;
using PyCore::read_int; using PyCore::read_list;
using PyCore::InstView; using PyCore::inst_view; using PyCore::inst_attr;

static bool attr_view(const InstView &v, int64_t &idx, const char *name, InstView &out)
{
    bool ok;
    uint64_t o = inst_attr(v, idx, name, ok);
    return ok && inst_view(o, out);
}

static bool is_dict(uint64_t o) { return is_obj(o) && getPtr64(o + 8) == PyRoot::g_dict_type; }

// 读一条 pred(普通 dict，int 键)。只要 PS 和 TARGET 读得出来就收
static bool read_pred(uint64_t d, Pred &out, int &ps)
{
    Dict k;
    if (!is_dict(d) || !read_dict(d, k) || k.n_entries > 32) return false;
    out.action = -1; out.sub = 0; out.use_id = -1; out.tn = 0;
    ps = -1;
    bool have_tgt = false;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (kp == 0) continue;
        int64_t key = -1;
        if (!read_int(kp, key)) continue;
        uint64_t vp = getPtr64(value_slot(k, i));
        int64_t v = 0;
        switch (key) {
        case K_ACTION:     if (read_int(vp, v)) out.action = (int)v; break;
        case K_PS:         if (read_int(vp, v)) ps = (int)v; break;
        case K_USE_ID:     if (read_int(vp, v)) out.use_id = v; break;
        case K_USE_SUB_ID: if (read_int(vp, v)) out.sub = (int)v; break;
        case K_TARGET: {
            int64_t n = 0; uint64_t items = 0;
            if (!read_list(vp, n, items)) break;
            have_tgt = true;
            for (int64_t t = 0; t < n && out.tn < MAX_TARGETS; t++) {
                int64_t u = -1;
                if (read_int(getPtr64(items + t * 8), u)) out.tgt[out.tn++] = u;
            }
            break;
        }
        default: break;
        }
    }
    return ps >= 0 && have_tgt;
}

static bool try_refresh(Snapshot &res)
{
    if (g_module_dict == 0) {
        const char *why = "";
        g_module_dict = PyCore::game_kernel_dict(why);
        if (!g_module_dict) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }
    }

    // GK.g_world_mgr.cur_world.prophet_mgr.behavior_info
    g_i_world_mgr = find_key(g_module_dict, "g_world_mgr", g_i_world_mgr);
    InstView wv, cv, pv;
    if (!inst_view(g_i_world_mgr >= 0 ? get_attr(g_module_dict, g_i_world_mgr) : 0, wv)) {
        snprintf(g_status, sizeof(g_status), "%s", T("g_world_mgr 无效", "g_world_mgr invalid")); return false;
    }
    if (!(attr_view(wv, g_i_cur_world, "cur_world", cv) && attr_view(cv, g_i_prophet, "prophet_mgr", pv))) {
        int64_t tmp = -1;
        if (!(attr_view(wv, g_i_battle_world, "battle_world", cv) && attr_view(cv, tmp, "prophet_mgr", pv))) {
            snprintf(g_status, sizeof(g_status), "%s", T("没有 prophet_mgr(不在对局中?)", "no prophet_mgr (not in match?)"));
            return false;
        }
    }
    bool ok;
    uint64_t binfo = inst_attr(pv, g_i_binfo, "behavior_info", ok);
    Dict bk;
    if (!ok || !is_dict(binfo) || !read_dict(binfo, bk)) {
        snprintf(g_status, sizeof(g_status), "%s", T("behavior_info 读不到", "behavior_info unreadable")); return false;
    }

    res.main_uid = -1;
    g_i_gunit = find_key(g_module_dict, "g_unit", g_i_gunit);
    InstView gv;
    if (inst_view(g_i_gunit >= 0 ? get_attr(g_module_dict, g_i_gunit) : 0, gv)) {
        uint64_t v = inst_attr(gv, g_i_gunit_uid, "uid", ok);
        int64_t u = -1;
        if (ok && read_int(v, u)) res.main_uid = u;
    }

    res.n = 0;
    int total = 0;
    for (int64_t i = 0; i < bk.n_entries && bk.n_entries <= 16; i++) {
        uint64_t kp = getPtr64(key_slot(bk, i));
        if (kp == 0) continue;
        int64_t kuid = -1;
        if (!read_int(kp, kuid)) continue;
        int64_t n = 0; uint64_t items = 0;
        if (!read_list(getPtr64(value_slot(bk, i)), n, items)) continue;
        for (int64_t j = 0; j < n; j++) {
            Pred p; int ps = -1;
            if (!read_pred(getPtr64(items + j * 8), p, ps)) continue;
            total++;
            if (ps != 2 && ps != 3) continue;            // 只要"预测中"，对/错不显示
            if (res.n >= MAX_PREDS) break;
            p.knight_uid = kuid;
            res.p[res.n++] = p;
        }
    }
    snprintf(g_status, sizeof(g_status), T("正常 记录%d 预测中%d", "OK records %d active %d"), total, res.n);
    return true;
}

static const int REFRESH_INTERVAL_MS = 50;

// 新出现的预测打一行日志(上机判断服务端有没有把 ACTION 发给监管端)
static void log_new_preds(const Snapshot &s)
{
    static int64_t seen[32]; static int seen_n = 0;
    for (int i = 0; i < s.n; i++) {
        const Pred &p = s.p[i];
        int64_t key = (p.knight_uid << 20) ^ (p.use_id << 4) ^ p.sub;
        bool dup = false;
        for (int k = 0; k < seen_n; k++) if (seen[k] == key) { dup = true; break; }
        if (dup) continue;
        if (seen_n >= 32) seen_n = 0;
        seen[seen_n++] = key;
        printf("[骑士] 预测 骑士uid=%lld 动作=%d 段=%d 目标=%lld 我=%lld\n", (long long)p.knight_uid, p.action, p.sub,
               (long long)(p.tn > 0 ? p.tgt[0] : -1), (long long)s.main_uid);
        fflush(stdout);
    }
}

static void thread_main()
{
    static Snapshot work;
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
                log_new_preds(work);
            } else if (++g_fail_streak >= 8) {           // 连续失败：序号缓存作废，下一轮按名字重找
                g_module_dict = 0;
                g_i_world_mgr = g_i_cur_world = g_i_battle_world = g_i_prophet = g_i_binfo = -1;
                g_i_gunit = g_i_gunit_uid = -1;
                g_fail_streak = 0;
            }
        } else if (g_libbase) {
            snprintf(g_status, sizeof(g_status), "%s", PyRoot::status_text());
        }
        g_available = ok;
        if (!ok && strcmp(logged, g_status) != 0) {
            printf("[骑士] %s\n", g_status); fflush(stdout);
            snprintf(logged, sizeof(logged), "%s", g_status);
        } else if (ok) {
            logged[0] = 0;
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
    printf("[骑士] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------
static inline bool available() { return g_available.load(); }
static inline const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

static inline const char *action_name(int action)
{
    switch (action >> 8) {
    case 2: return T("普攻", "Attack");
    case 3: return T("技能", "Skill");
    case 4: return T("相持", "Standoff");
    default: return nullptr;
    }
}

// 针对我(uid 为 me 或主角色)的预测名字，第一段在前，返回个数(0 = 没有)。间距由绘制侧决定
static int names_for(int64_t me, const char **out, int cap)
{
    if (!g_available.load()) return 0;
    const Snapshot &s = g_buf[g_active.load()];
    const int64_t main = s.main_uid;
    const Pred *sel[MAX_PREDS]; int m = 0;
    for (int i = 0; i < s.n; i++) {
        const Pred &p = s.p[i];
        if (!action_name(p.action)) continue;
        bool mine = false;
        for (int t = 0; t < p.tn; t++)
            if ((me > 0 && p.tgt[t] == me) || (main > 0 && p.tgt[t] == main)) { mine = true; break; }
        if (mine) sel[m++] = &p;
    }
    for (int a = 1; a < m; a++)                          // 第一段在前
        for (int b = a; b > 0 && sel[b]->sub < sel[b - 1]->sub; b--) { const Pred *t = sel[b]; sel[b] = sel[b - 1]; sel[b - 1] = t; }
    int k = 0;
    for (int i = 0; i < m && k < cap; i++) out[k++] = action_name(sel[i]->action);
    return k;
}

} // namespace PyKnight

#endif // IDV_PY_KNIGHT_H
