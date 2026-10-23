#pragma once
// ============================================================================
// PyScene.h —— 游戏阶段(大厅 / 准备 / 局内)判定，单一真相源
// ============================================================================
// 判据实现在 PyCore::detect_phase()：读 g_ui_mgr.ui_dict 里挂着哪个 UI 对象
//   大厅 = UIHall   准备 = UIRoomWait   局内 = UIBattle(另带 UIControlCustom)
// 来历与跨进程实测见 idv-reverse-resume SKILL「区分三个阶段」。这是 007 的 domain 域选择模式。
//
// 独立线程 500ms 一轮(阶段切换不需要更勤)，原子发布，和 PyProgress/PyGenius 一个套路。
// 绘制侧用 phase()/in_battle()/in_prepare()/in_lobby() 读，便宜(只走几跳 + 扫一个 1~2 key 的小 dict)。
// 跟已有的 units_by_type 键 1/2(uses_battle_world)可互为交叉校验。

#include <atomic>
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include "PyCore.h"

namespace PyScene {

static uintptr_t         g_libbase = 0;
static std::atomic<int>  g_phase{PyCore::PHASE_UNKNOWN};   // 见 PyProgress.h 同名变量注释(原子保证发布)
static char              g_status[96] = "";   // 空 = 未启动，见 status_text()

static void refresh_once()
{
    if (g_libbase == 0) return;
    if (!PyRoot::ensure()) {                    // CPython 根还没就绪(热更后重定位中/页换出)
        g_phase = PyCore::PHASE_UNKNOWN;
        snprintf(g_status, sizeof(g_status), "%s", PyRoot::status_text());
        return;
    }
    const char *reason = "";
    PyCore::Phase ph = PyCore::detect_phase(reason);
    g_phase = (int)ph;
    static const char *NAME[] = {"未知", "大厅", "准备", "局内"};
    static const char *NAME_EN[] = {"unknown", "lobby", "prep", "in match"};
    snprintf(g_status, sizeof(g_status), "%s (%s)", (g_english ? NAME_EN : NAME)[(int)ph & 3], reason);
}

static const int REFRESH_INTERVAL_MS = 500;

static void thread_main()
{
    while (true) {
        refresh_once();
        usleep(REFRESH_INTERVAL_MS * 1000);
    }
}

static void start(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase = libbase;
    snprintf(g_status, sizeof(g_status), "%s", T("启动中", "starting"));
    std::thread(thread_main).detach();
    printf("[阶段] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------
static inline PyCore::Phase phase()       { return (PyCore::Phase)g_phase.load(); }
static inline bool          in_battle()   { return g_phase.load() == PyCore::PHASE_BATTLE; }
static inline bool          in_prepare()  { return g_phase.load() == PyCore::PHASE_PREPARE; }
static inline bool          in_lobby()    { return g_phase.load() == PyCore::PHASE_LOBBY; }
static inline const char   *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

} // namespace PyScene
