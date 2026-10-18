#pragma once
// ============================================================================
// GeniusTree.h —— 监管天赋树的拓扑与径向布局(画"内在人格脉络"那棵树用)
// ============================================================================
// 拓扑来自 GK.DM.butcher_genius 的 pre_genius / layer / node_seq，
// cmd_genius_topo.py 实测逐条 dump(见 idv-replay-genius)。配表里没有坐标字段，
// 所以这里按"4 分支各占一个扇区 + 树深决定半径"自己算放射状布局，一次算好缓存。
// 节点亮灭/等级由调用方传入的 Info.genius_levels[1..40] 决定，这里只管几何。

#include <cmath>

namespace GeniusTree {

static const int N = 41;                         // 下标 1~40，0 占位

// 父节点 id(0=分支根)。1~8 / 9~16 / 17~24 / 25~32 四分支，33~40 是挂在各支 2.3 节点上的额外节点。
static const int PARENT[N] = {
    0,
    0, 1, 1, 1,  3, 4, 4, 5,      // 1..8   根1;  2←1 3←1 4←1; 5←3 6←4 7←4; 8←5(终端)
    0, 9, 9, 9, 11,12,12,13,      // 9..16
    0,17,17,17, 19,20,20,21,      // 17..24
    0,25,25,25, 27,28,28,29,      // 25..32
    2,26,18,10, 26, 2,18,10       // 33..40  (seq4: 33←2 34←26 35←18 36←10; seq5: 37←26 38←2 39←18 40←10)
};

// node_seq，子节点排序用(越小越靠扇区一侧，保证布局稳定、终端居中)
static const int NSEQ[N] = {
    0,
    1,3,2,1, 3,2,1,1,
    1,3,2,1, 3,2,1,1,
    1,3,2,1, 3,2,1,1,
    1,3,2,1, 3,2,1,1,
    4,4,4,4, 5,5,5,5
};

static float g_px[N], g_py[N];   // 单位坐标(中心为原点、y 向下)；draw 时乘 scale + 平移到中心
static int   g_depth[N];
static float g_ang[N];
static bool  g_ready = false;

// --- 布局计算时用的分支临时状态 ---
static float g_base, g_half;
static int   g_leaf_slot, g_leaf_total;

static int children(int id, int *out)
{
    int c = 0;
    for (int k = 1; k < N; k++) if (PARENT[k] == id) out[c++] = k;
    for (int i = 0; i < c; i++)                         // 按 node_seq 冒泡(节点少，够用)
        for (int j = i + 1; j < c; j++)
            if (NSEQ[out[j]] < NSEQ[out[i]]) { int t = out[i]; out[i] = out[j]; out[j] = t; }
    return c;
}

static int count_leaves(int id)
{
    int ch[8]; int nc = children(id, ch);
    if (nc == 0) return 1;
    int s = 0; for (int i = 0; i < nc; i++) s += count_leaves(ch[i]);
    return s;
}

// 叶子按槽位均分到扇区；内部节点取子节点角度均值。返回并缓存该节点角度。
static float layout_angle(int id)
{
    int ch[8]; int nc = children(id, ch);
    float a;
    if (nc == 0) {
        a = g_base - g_half + (g_leaf_slot + 0.5f) / g_leaf_total * (2.0f * g_half);
        g_leaf_slot++;
    } else {
        float sum = 0.0f;
        for (int i = 0; i < nc; i++) sum += layout_angle(ch[i]);
        a = sum / nc;
    }
    g_ang[id] = a;
    return a;
}

static void ensure()
{
    if (g_ready) return;
    const float PI = 3.14159265f;
    for (int id = 1; id < N; id++) {               // 树深 = 到根的跳数
        int d = 0, x = id;
        while (PARENT[x]) { x = PARENT[x]; d++; }
        g_depth[id] = d;
    }
    const int roots[4] = {1, 9, 17, 25};
    for (int b = 0; b < 4; b++) {                   // 四支分别占 上/右/下/左 一个扇区
        g_base = -PI / 2.0f + b * (PI / 2.0f);
        g_half = 40.0f * PI / 180.0f;               // 扇区半宽 40°(留 10° 间隙)
        g_leaf_total = count_leaves(roots[b]);
        g_leaf_slot = 0;
        layout_angle(roots[b]);
    }
    for (int id = 1; id < N; id++) {
        float r = 1.0f + (float)g_depth[id];        // 单位半径：根=1，每深一层 +1(终端 depth3=4)
        g_px[id] = r * cosf(g_ang[id]);
        g_py[id] = r * sinf(g_ang[id]);
    }
    g_ready = true;
}

static inline bool is_terminal(int id) { return id == 8 || id == 16 || id == 24 || id == 32; }

} // namespace GeniusTree
