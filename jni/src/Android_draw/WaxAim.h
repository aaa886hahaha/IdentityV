#pragma once
// ============================================================================
// WaxAim.h —— 蜡像师喷蜡辅助瞄准(2026-11-04)
// ============================================================================
// 依据：游戏脚本离线反编译(common_cs/skills/wax/*、skills/scs/AirPowerWax、LAirPower、CollisionMgr)，
// 调研全文见 idv-reverse-resume/wax-aim.md。这里只复刻服务端的判定，不调用游戏任何方法、不写内存。
//
// 弹道(服务端，逻辑帧 1/30 s × tick_times 3 = 子步 1/90 s)：
//   出生点 = 监管脚底 + 右 2 + 上 16 + 前 3；仰角 = -last_pitch + 12；初速 300、水平阻力 6、重力 50
//   每子步: vy -= g*dt; vx -= 6*dt; dir = normalize(H*vx + UP*vy); pos += dir*vx*dt + UP*vy*dt
//   (竖直分量算了两遍，不是标准抛物线；按代码逐步积分就是真值)
// 命中：场景球扫半径 1；单位球扫半径 5，单位命中优先(扫到场景命中点为止)。
//   密码机两块盒子(半长)：机身 (5,8,5) 中心 +8 高；天线 (5,8,5) 偏移 (-8, 78|室内53, 5)，x = up×朝向
// 遮挡：服务端静态碰撞 .scn_col + 服务端空气墙打成资源包 jni/src/res/wax_col.pak(analysis/wax/pack_col.py)，
//   编译时 .incbin 嵌进可执行文件，按 scene_id 解压当前图(只有排位 9 张图)。
//   求生者挡路不算挡(用户要求)：照样算能附着，只是那段瞄准线画绿。
//
// 线程：自己一个线程(33ms)。读 Python(相机俯仰/喷射限制区间/pid/室内标记/求生者位置/scene_id)、算瞄准区间、
// 预测轨迹，双缓冲发布。绘制线程只投影和画。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <zlib.h>
#include "PyRoot.h"
#include "PyCore.h"
#include "PySelf.h"
#include "PyProgress.h"

namespace WaxAim {

// ---------------- 菜单项 ----------------
inline bool enabled       = true;    // 总开关(只在我是蜡像师时生效)
inline bool show_traj     = true;    // 落点预测(喷射中，只画终点的小球)
inline bool show_always   = false;   // 不喷射时也画瞄准线
inline float aim_x = -1.f, aim_y = -1.f;   // 游戏蜡枪瞄准标记的屏幕坐标，<0 = 屏幕中心

// ---------------- 常量(配表 skill 3811 / air_power 36 / box_bounding 3,150,151) ----------------
constexpr float SPEED = 300.f, RESIST = 6.f, GRAV = 50.f, PITCH_ADD = 12.f;
constexpr float OFF_R = 2.f, OFF_U = 16.f, OFF_F = 3.f;
constexpr float DT = 1.f / 90.f;
constexpr int   MAX_STEPS = 420;                    // 4.67 s
constexpr float R_SCENE = 1.f, R_UNIT = 5.f;
constexpr float TBL_LO = -10.f, TBL_HI = 40.f, TBL_STEP = 0.1f;
constexpr int   TBL_N = (int)((TBL_HI - TBL_LO) / TBL_STEP + 1.5f);
constexpr float DEF_LMT_LO = -3.f, DEF_LMT_HI = 30.f;   // 喷射时相机俯仰限制(arg2 = '0,-3,30')
constexpr float MAX_DIST = 90.f * 11.886f;              // 90 米外不算
constexpr int   WAX_PID = 112;

// ---------------- 发布给绘制线程的结果 ----------------
struct Band { float hx, hz, half_yaw, p0, p1, dist_m; uint8_t part, surv; };  // p0<p1: 相机俯仰区间
static const int MAX_BANDS = 96, MAX_GEN = 16, MAX_PTS = 160;
struct Box { float c[3], ax[3], az[3], h[3]; };        // 中心、局部 x/z 轴、半长(未外扩)
struct Result {
    bool is_wax = false, spraying = false;
    float last_pitch = 0.f, lmt0 = DEF_LMT_LO, lmt1 = DEF_LMT_HI;
    int nb = 0; Band b[MAX_BANDS];
    int ng = 0; uint64_t gen_obj[MAX_GEN]; bool gen_green[MAX_GEN];
    int end_kind = 0; float end[3] = {0, 0, 0};                  // 落点：0 没算 1 机身 2 天线 3 墙/地 4 超时没落地
};
static Result g_res[2];
static std::atomic<int> g_res_active{0};

// 绘制线程每帧写：相机水平朝向 + 当前视线仰角(由 VP 矩阵算)
static std::atomic<float> g_cam_fx{0.f}, g_cam_fz{1.f}, g_cam_elev{0.f};
static std::atomic<bool>  g_cam_ok{false};
inline void set_view(float fx, float fz, float elev_deg)
{
    g_cam_fx = fx; g_cam_fz = fz; g_cam_elev = elev_deg; g_cam_ok = true;
}

// ---------------- 弹道表：只依赖俯仰，跟目标无关，启动时算一次 ----------------
struct Traj { int n; float f[MAX_STEPS + 1], u[MAX_STEPS + 1]; };   // f 沿水平朝向，u 竖直，都相对出生点
static Traj *g_tbl = nullptr;

static void build_table()
{
    g_tbl = new Traj[TBL_N];
    const float max_rot = 120.f / 180.f * (float)M_PI / 3.f;    // global_data.max_rotate_angle / tick_times
    for (int i = 0; i < TBL_N; i++) {
        float cam = TBL_LO + i * TBL_STEP;
        float a = (-cam + PITCH_ADD) * (float)M_PI / 180.f;
        float vx = SPEED * cosf(a), vy = SPEED * sinf(a);
        float x = 0, y = 0, dx = 1, dy = 0;
        Traj &t = g_tbl[i];
        t.f[0] = 0; t.u[0] = 0; t.n = 1;
        for (int s = 1; s <= MAX_STEPS; s++) {
            vy -= DT * GRAV;
            if (vx > 0) { vx -= DT * RESIST; if (vx < 0) vx = 0; }
            float tx = vx * DT, ty = vy * DT, n = sqrtf(tx * tx + ty * ty);
            if (n > 0) {
                float ndx = tx / n, ndy = ty / n;
                float dot = fmaxf(-1.f, fminf(1.f, dx * ndx + dy * ndy));
                float rad = acosf(dot);
                if (rad <= max_rot) { dx = ndx; dy = ndy; }
                else {
                    float cur = atan2f(dy, dx), tgt = atan2f(ndy, ndx);
                    cur += copysignf(max_rot, tgt - cur);
                    dx = cosf(cur); dy = sinf(cur);
                }
            }
            x += dx * vx * DT;
            y += dy * vx * DT + vy * DT;
            t.f[s] = x; t.u[s] = y; t.n = s + 1;
            if (y < -400.f) break;
        }
    }
}

static inline int tbl_index(float cam)
{
    int i = (int)lroundf((cam - TBL_LO) / TBL_STEP);
    return i < 0 ? 0 : (i >= TBL_N ? TBL_N - 1 : i);
}

// ---------------- 几何 ----------------
static inline void v3sub(const float *a, const float *b, float *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static inline float v3dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void v3cross(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 线段 p1q1 与 p2q2 的最近距离平方(Ericson 5.1.9)
static float seg_seg_d2(const float *p1, const float *q1, const float *p2, const float *q2)
{
    float d1[3], d2[3], r[3];
    v3sub(q1, p1, d1); v3sub(q2, p2, d2); v3sub(p1, p2, r);
    float a = v3dot(d1, d1), e = v3dot(d2, d2), f = v3dot(d2, r), s, t;
    const float EPS = 1e-8f;
    if (a <= EPS && e <= EPS) { return v3dot(r, r); }
    if (a <= EPS) { s = 0; t = clampf(f / e, 0, 1); }
    else {
        float c = v3dot(d1, r);
        if (e <= EPS) { t = 0; s = clampf(-c / a, 0, 1); }
        else {
            float b = v3dot(d1, d2), den = a * e - b * b;
            s = den != 0 ? clampf((b * f - c * e) / den, 0, 1) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = clampf(-c / a, 0, 1); }
            else if (t > 1) { t = 1; s = clampf((b - c) / a, 0, 1); }
        }
    }
    float c1[3] = {p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s};
    float c2[3] = {p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t};
    float w[3]; v3sub(c1, c2, w);
    return v3dot(w, w);
}

// 点到三角形最近距离平方(Ericson 5.1.5)
static float pt_tri_d2(const float *p, const float *a, const float *b, const float *c)
{
    float ab[3], ac[3], ap[3], q[3];
    v3sub(b, a, ab); v3sub(c, a, ac); v3sub(p, a, ap);
    float d1 = v3dot(ab, ap), d2 = v3dot(ac, ap);
    auto out = [&](const float *x) { float w[3]; v3sub(p, x, w); return v3dot(w, w); };
    if (d1 <= 0 && d2 <= 0) return out(a);
    float bp[3]; v3sub(p, b, bp);
    float d3 = v3dot(ab, bp), d4 = v3dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return out(b);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v; return out(q); }
    float cp[3]; v3sub(p, c, cp);
    float d5 = v3dot(ab, cp), d6 = v3dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return out(c);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); for (int k = 0; k < 3; k++) q[k] = a[k] + ac[k] * w; return out(q); }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) q[k] = b[k] + (c[k] - b[k]) * w; return out(q);
    }
    float den = 1.f / (va + vb + vc), v = vb * den, w = vc * den;
    for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;
    return out(q);
}

// 线段穿过三角形(Möller–Trumbore，限制在线段内)
static bool seg_tri_cross(const float *p, const float *q, const float *a, const float *b, const float *c)
{
    float e1[3], e2[3], d[3], h[3], s[3], qq[3];
    v3sub(b, a, e1); v3sub(c, a, e2); v3sub(q, p, d);
    v3cross(d, e2, h);
    float det = v3dot(e1, h);
    if (fabsf(det) < 1e-9f) return false;
    float inv = 1.f / det;
    v3sub(p, a, s);
    float u = v3dot(s, h) * inv;
    if (u < 0 || u > 1) return false;
    v3cross(s, e1, qq);
    float v = v3dot(d, qq) * inv;
    if (v < 0 || u + v > 1) return false;
    float t = v3dot(e2, qq) * inv;
    return t >= 0 && t <= 1;
}

static bool seg_tri_within(const float *p, const float *q, const float *t, float r)
{
    const float *a = t, *b = t + 3, *c = t + 6;
    float r2 = r * r;
    if (seg_tri_cross(p, q, a, b, c)) return true;
    if (pt_tri_d2(p, a, b, c) <= r2 || pt_tri_d2(q, a, b, c) <= r2) return true;
    return seg_seg_d2(p, q, a, b) <= r2 || seg_seg_d2(p, q, b, c) <= r2 || seg_seg_d2(p, q, c, a) <= r2;
}

// 线段进入外扩 OBB 的参数 t(0~1)，没进入返回 -1。e = 外扩量
static float seg_obb_enter(const float *p, const float *q, const Box &bx, float e)
{
    const float ay[3] = {0, 1, 0};
    const float *axes[3] = {bx.ax, ay, bx.az};
    float rp[3];
    v3sub(p, bx.c, rp);
    float d[3]; v3sub(q, p, d);
    float t0 = 0, t1 = 1;
    for (int k = 0; k < 3; k++) {
        float o = v3dot(rp, axes[k]), dd = v3dot(d, axes[k]), h = bx.h[k] + e;
        if (fabsf(dd) < 1e-9f) { if (o < -h || o > h) return -1; continue; }
        float ta = (-h - o) / dd, tb = (h - o) / dd;
        if (ta > tb) { float x = ta; ta = tb; tb = x; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return -1;
    }
    return t0;
}

// ---------------- 碰撞网格：编译时嵌进可执行文件的资源包 ----------------
// jni/src/res/wax_col.pak 由 analysis/wax/pack_col.py 生成(排位 9 张图的服务端 .scn_col + 服务端空气墙)，
// 这里用 .incbin 原样塞进只读段，不用再往手机推文件。格式见 pack_col.py 文件头。
// .incbin 的路径相对编译时的当前目录 = 工程根(1.bat 在工程根跑 ndk-build)。
// ⚠ 只换了 .pak 而没改源码时 ndk-build 不会重编 draw_Gui.o，要么改一下这个文件，要么删 obj/ 再编
__asm__(
    ".section .rodata.wax_col,\"a\",%progbits\n"
    ".balign 16\n"
    ".global g_wax_col_pak\n"
    ".hidden g_wax_col_pak\n"
    "g_wax_col_pak:\n"
    ".incbin \"jni/src/res/wax_col.pak\"\n"
    ".global g_wax_col_pak_end\n"
    ".hidden g_wax_col_pak_end\n"
    "g_wax_col_pak_end:\n"
    ".previous\n");
extern "C" const unsigned char g_wax_col_pak[];
extern "C" const unsigned char g_wax_col_pak_end[];

struct ColMesh {
    bool ok = false; int scene = -1;
    std::vector<float> tri;            // 9 / 三角形
    std::vector<float> tb;             // 6 / 三角形：minx maxx miny maxy minz maxz
    float minx = 0, minz = 0, cell = 16; uint32_t gw = 0, gh = 0;
    std::vector<uint32_t> start, idx;
    std::vector<uint32_t> mark; uint32_t stamp = 0;
};
static ColMesh g_col;                  // 只有 WaxAim 线程碰
static char g_col_status[96] = "";

template <class T> static inline T pak_rd(const unsigned char *p) { T v; memcpy(&v, p, sizeof(T)); return v; }

static bool load_col(int scene)
{
    g_col = ColMesh();
    g_col.scene = scene;
    const unsigned char *P = g_wax_col_pak;
    const size_t N = (size_t)(g_wax_col_pak_end - g_wax_col_pak);
    auto fail = [&](const char *zh, const char *en) {
        g_col = ColMesh(); g_col.scene = scene;
        snprintf(g_col_status, sizeof(g_col_status), T(zh, en), scene);
        return false;
    };
    if (N < 20 || memcmp(P, "WPAK", 4) != 0 || pak_rd<uint32_t>(P + 4) != 1) return fail("场景%d：碰撞资源包损坏", "scene %d: collision pack corrupt");
    const uint32_t n_mesh = pak_rd<uint32_t>(P + 8), n_scene = pak_rd<uint32_t>(P + 12), n_extra = pak_rd<uint32_t>(P + 16);
    const size_t mesh_tab = 20, scene_tab = mesh_tab + (size_t)n_mesh * 40, extra_tab = scene_tab + (size_t)n_scene * 16;
    if (extra_tab + (size_t)n_extra * 36 > N) return fail("场景%d：碰撞资源包损坏", "scene %d: collision pack corrupt");

    int mi = -1; uint32_t ex_first = 0, ex_n = 0;
    for (uint32_t i = 0; i < n_scene; i++) {
        const unsigned char *e = P + scene_tab + (size_t)i * 16;
        if (pak_rd<int32_t>(e) == scene) { mi = pak_rd<int32_t>(e + 4); ex_first = pak_rd<uint32_t>(e + 8); ex_n = pak_rd<uint32_t>(e + 12); break; }
    }
    if (mi < 0 || (uint32_t)mi >= n_mesh || ex_first + ex_n > n_extra)
        return fail("场景%d 不是排位图，不判遮挡", "scene %d is not a ranked map, no occlusion");

    const unsigned char *m = P + mesh_tab + (size_t)mi * 40;
    const uint32_t off = pak_rd<uint32_t>(m), csize = pak_rd<uint32_t>(m + 4), nvert = pak_rd<uint32_t>(m + 8), ntri = pak_rd<uint32_t>(m + 12);
    float mn[3], st[3];
    memcpy(mn, m + 16, 12); memcpy(st, m + 28, 12);
    if ((size_t)off + csize > N || nvert == 0 || ntri == 0 || ntri > 2000000) return fail("场景%d：碰撞资源包损坏", "scene %d: collision pack corrupt");

    std::vector<unsigned char> raw((size_t)nvert * 6 + (size_t)ntri * 12);
    uLongf rl = (uLongf)raw.size();
    if (uncompress(raw.data(), &rl, P + off, csize) != Z_OK || rl != raw.size()) return fail("场景%d：碰撞数据解压失败", "scene %d: collision inflate failed");

    const uint32_t total = ntri + ex_n;
    g_col.tri.resize((size_t)total * 9);
    const unsigned char *qv = raw.data(), *ix = raw.data() + (size_t)nvert * 6;
    for (uint32_t i = 0; i < ntri; i++) {
        float *t = &g_col.tri[(size_t)i * 9];
        for (int k = 0; k < 3; k++) {
            uint32_t vi = pak_rd<uint32_t>(ix + ((size_t)i * 3 + k) * 4);
            if (vi >= nvert) return fail("场景%d：碰撞资源包损坏", "scene %d: collision pack corrupt");
            for (int a = 0; a < 3; a++) t[k * 3 + a] = mn[a] + (float)pak_rd<uint16_t>(qv + (size_t)vi * 6 + a * 2) * st[a];
        }
    }
    memcpy(&g_col.tri[(size_t)ntri * 9], P + extra_tab + (size_t)ex_first * 36, (size_t)ex_n * 36);   // 服务端空气墙

    // 三角形包围盒 + XZ 均匀网格(16 单位一格)，两遍：先数每格多少个，再填
    g_col.tb.resize((size_t)total * 6);
    float lo_x = 1e30f, hi_x = -1e30f, lo_z = 1e30f, hi_z = -1e30f;
    for (uint32_t i = 0; i < total; i++) {
        const float *t = &g_col.tri[(size_t)i * 9];
        float *b = &g_col.tb[(size_t)i * 6];
        b[0] = fminf(t[0], fminf(t[3], t[6])); b[1] = fmaxf(t[0], fmaxf(t[3], t[6]));
        b[2] = fminf(t[1], fminf(t[4], t[7])); b[3] = fmaxf(t[1], fmaxf(t[4], t[7]));
        b[4] = fminf(t[2], fminf(t[5], t[8])); b[5] = fmaxf(t[2], fmaxf(t[5], t[8]));
        lo_x = fminf(lo_x, b[0]); hi_x = fmaxf(hi_x, b[1]); lo_z = fminf(lo_z, b[4]); hi_z = fmaxf(hi_z, b[5]);
    }
    g_col.minx = lo_x; g_col.minz = lo_z;
    g_col.gw = (uint32_t)((hi_x - lo_x) / g_col.cell) + 1;
    g_col.gh = (uint32_t)((hi_z - lo_z) / g_col.cell) + 1;
    if ((uint64_t)g_col.gw * g_col.gh > 4000000) return fail("场景%d：地图范围异常", "scene %d: map bounds abnormal");
    g_col.start.assign((size_t)g_col.gw * g_col.gh + 1, 0);
    auto cells = [&](uint32_t i, uint32_t &x0, uint32_t &x1, uint32_t &z0, uint32_t &z1) {
        const float *b = &g_col.tb[(size_t)i * 6];
        x0 = (uint32_t)((b[0] - lo_x) / g_col.cell); x1 = (uint32_t)((b[1] - lo_x) / g_col.cell);
        z0 = (uint32_t)((b[4] - lo_z) / g_col.cell); z1 = (uint32_t)((b[5] - lo_z) / g_col.cell);
        if (x1 >= g_col.gw) x1 = g_col.gw - 1;
        if (z1 >= g_col.gh) z1 = g_col.gh - 1;
    };
    for (uint32_t i = 0; i < total; i++) {
        uint32_t x0, x1, z0, z1; cells(i, x0, x1, z0, z1);
        for (uint32_t z = z0; z <= z1; z++) for (uint32_t x = x0; x <= x1; x++) g_col.start[z * g_col.gw + x + 1]++;
    }
    for (size_t c = 1; c < g_col.start.size(); c++) g_col.start[c] += g_col.start[c - 1];
    g_col.idx.resize(g_col.start.back());
    std::vector<uint32_t> fillp(g_col.start.begin(), g_col.start.end() - 1);
    for (uint32_t i = 0; i < total; i++) {
        uint32_t x0, x1, z0, z1; cells(i, x0, x1, z0, z1);
        for (uint32_t z = z0; z <= z1; z++) for (uint32_t x = x0; x <= x1; x++) g_col.idx[fillp[z * g_col.gw + x]++] = i;
    }
    g_col.mark.assign(total, 0);
    g_col.ok = true;
    snprintf(g_col_status, sizeof(g_col_status), T("场景%d %u 三角", "scene %d %u tris"), scene, total);
    return true;
}

// 线段 p->q(半径 r 的球扫)第一次碰到静态场景的参数 t(按 4 段细分，0/0.25/0.5/0.75)，没碰到返回 -1
static float scene_hit(const float *p, const float *q, float r)
{
    if (!g_col.ok) return -1;
    const int SUB = 4;
    float best = -1;
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++) { lo[k] = fminf(p[k], q[k]) - r; hi[k] = fmaxf(p[k], q[k]) + r; }
    int x0 = (int)floorf((lo[0] - g_col.minx) / g_col.cell), x1 = (int)floorf((hi[0] - g_col.minx) / g_col.cell);
    int z0 = (int)floorf((lo[2] - g_col.minz) / g_col.cell), z1 = (int)floorf((hi[2] - g_col.minz) / g_col.cell);
    if (x1 < 0 || z1 < 0 || x0 >= (int)g_col.gw || z0 >= (int)g_col.gh) return -1;
    x0 = x0 < 0 ? 0 : x0; z0 = z0 < 0 ? 0 : z0;
    x1 = x1 >= (int)g_col.gw ? (int)g_col.gw - 1 : x1; z1 = z1 >= (int)g_col.gh ? (int)g_col.gh - 1 : z1;
    if (++g_col.stamp == 0) { std::fill(g_col.mark.begin(), g_col.mark.end(), 0); g_col.stamp = 1; }
    for (int gz = z0; gz <= z1; gz++)
        for (int gx = x0; gx <= x1; gx++) {
            uint32_t ci = (uint32_t)gz * g_col.gw + gx;
            for (uint32_t j = g_col.start[ci]; j < g_col.start[ci + 1]; j++) {
                uint32_t ti = g_col.idx[j];
                if (g_col.mark[ti] == g_col.stamp) continue;
                g_col.mark[ti] = g_col.stamp;
                const float *b = &g_col.tb[(size_t)ti * 6];
                if (b[1] < lo[0] || b[0] > hi[0] || b[3] < lo[1] || b[2] > hi[1] || b[5] < lo[2] || b[4] > hi[2]) continue;
                const float *t = &g_col.tri[(size_t)ti * 9];
                for (int s = 0; s < SUB; s++) {
                    float ts = (float)s / SUB;
                    if (best >= 0 && ts >= best) break;
                    float a[3], c[3];
                    for (int k = 0; k < 3; k++) { a[k] = p[k] + (q[k] - p[k]) * ts; c[k] = p[k] + (q[k] - p[k]) * (ts + 1.f / SUB); }
                    if (seg_tri_within(a, c, t, r)) { best = ts; break; }
                }
            }
        }
    return best;
}

// ---------------- 场上数据(线程内) ----------------
struct Gen { uint64_t obj; float pos[3]; float zx, zz; bool indoor; float progress; Box box[2]; };
struct Surv { float pos[3]; float r, h; };
static Gen  g_gen[MAX_GEN]; static int g_ngen = 0;
static Surv g_surv[8];      static int g_nsurv = 0;

static void make_boxes(Gen &g)
{
    float zx = g.zx, zz = g.zz;
    float xx = zz, xz = -zx;                         // up × z = (zz, 0, -zx)
    Box &b0 = g.box[0];
    b0.c[0] = g.pos[0]; b0.c[1] = g.pos[1] + 8.f; b0.c[2] = g.pos[2];
    Box &b1 = g.box[1];
    float hu = g.indoor ? 53.f : 78.f;
    b1.c[0] = g.pos[0] + xx * -8.f + zx * 5.f; b1.c[1] = g.pos[1] + hu; b1.c[2] = g.pos[2] + xz * -8.f + zz * 5.f;
    for (Box *b : {&b0, &b1}) {
        b->ax[0] = xx; b->ax[1] = 0; b->ax[2] = xz;
        b->az[0] = zx; b->az[1] = 0; b->az[2] = zz;
        b->h[0] = 5.f; b->h[1] = 8.f; b->h[2] = 5.f;
    }
}

// ---------------- 单条轨迹判定 ----------------
// target >= 0：只认这台机(画瞄准区间)；target < 0：所有机都认(预测轨迹)。
// 返回 0 没打到 1 机身 2 天线 3 墙/地 4 超时/出界。surv = 途中穿过求生者
struct TraceOut { int kind; bool surv; float end[3]; int gen; };
static int g_pts_n = 0; static float g_pts[MAX_PTS][3]; static uint8_t g_pflag[MAX_PTS];

static void trace(const Traj &tr, const float *O, float hx, float hz, int target, bool use_scene,
                  float floor_h, TraceOut &out, bool record, int s_from = 1, int s_to = MAX_STEPS)
{
    float rx = hz, rz = -hx;                         // 右 = up × 朝向
    float S[3] = {O[0] + rx * OFF_R + hx * OFF_F, O[1] + OFF_U, O[2] + rz * OFF_R + hz * OFF_F};
    out.kind = 4; out.surv = false; out.gen = -1;
    float prev[3] = {S[0] + hx * tr.f[s_from - 1], S[1] + tr.u[s_from - 1], S[2] + hz * tr.f[s_from - 1]};
    if (record) { g_pts_n = 0; memcpy(g_pts[g_pts_n], prev, 12); g_pflag[g_pts_n++] = 0; }
    int last = tr.n - 1 < s_to ? tr.n - 1 : s_to;
    for (int s = s_from; s <= last; s++) {
        float cur[3] = {S[0] + hx * tr.f[s], S[1] + tr.u[s], S[2] + hz * tr.f[s]};
        float tu = 2.f; int hit_g = -1, hit_p = 0;
        int g0 = target >= 0 ? target : 0, g1 = target >= 0 ? target + 1 : g_ngen;
        for (int g = g0; g < g1; g++)
            for (int p = 0; p < 2; p++) {
                float t = seg_obb_enter(prev, cur, g_gen[g].box[p], R_UNIT);
                if (t >= 0 && t < tu) { tu = t; hit_g = g; hit_p = p + 1; }
            }
        float ts = use_scene ? scene_hit(prev, cur, R_SCENE) : -1;
        if (!g_col.ok && ts < 0 && cur[1] < floor_h && prev[1] > cur[1]) ts = (prev[1] - floor_h) / (prev[1] - cur[1]);   // 无网格时按地面高兜底
        float tend = (hit_g >= 0 && (ts < 0 || tu <= ts)) ? tu : (ts >= 0 ? ts : 2.f);
        if (!out.surv)
            for (int k = 0; k < g_nsurv; k++) {
                const Surv &v = g_surv[k];
                float a[3] = {v.pos[0], v.pos[1], v.pos[2]}, b[3] = {v.pos[0], v.pos[1] + v.h, v.pos[2]};
                float rr = v.r + R_UNIT;
                float q[3]; for (int j = 0; j < 3; j++) q[j] = prev[j] + (cur[j] - prev[j]) * fminf(tend, 1.f);
                if (seg_seg_d2(prev, q, a, b) <= rr * rr) { out.surv = true; break; }
            }
        if (tend <= 1.f) {
            for (int j = 0; j < 3; j++) out.end[j] = prev[j] + (cur[j] - prev[j]) * tend;
            if (hit_g >= 0 && (ts < 0 || tu <= ts)) { out.kind = hit_p; out.gen = hit_g; }
            else out.kind = 3;
            if (record && g_pts_n < MAX_PTS) { memcpy(g_pts[g_pts_n], out.end, 12); g_pflag[g_pts_n++] = out.surv; }
            return;
        }
        if (record && (s % 3 == 0) && g_pts_n < MAX_PTS - 1) { memcpy(g_pts[g_pts_n], cur, 12); g_pflag[g_pts_n++] = out.surv; }
        memcpy(prev, cur, 12);
    }
    memcpy(out.end, prev, 12);
}

// ---------------- Python 读取 ----------------
using PyCore::is_obj; using PyCore::InstView; using PyCore::inst_view; using PyCore::inst_attr;
static int64_t h_cam = -1, h_unit = -1, h_handler = -1, h_lp = -1, h_lmt = -1, h_pid = -1;
static int64_t h_um = -1, h_ubt = -1, h_wm = -1, h_cw = -1, h_sid = -1;
static int64_t h_g_model = -1, h_g_indoor = -1, h_s_pos = -1, h_s_col = -1, h_s_model = -1;

// 对象的类型名(PyTypeObject.tp_name，+0x18 是 C 字符串指针)
static bool type_is(uint64_t obj, const char *name)
{
    if (!is_obj(obj)) return false;
    uint64_t tp = getPtr64(obj + 8);
    uint64_t np = is_obj(tp) ? getPtr64(tp + 0x18) : 0;
    char buf[16] = {0};
    size_t n = strlen(name);
    if (!np || n >= sizeof(buf) || !vm_readv(np, buf, n + 1)) return false;
    return memcmp(buf, name, n + 1) == 0;
}

static bool attr_view(const InstView &v, int64_t &idx, const char *name, InstView &out)
{
    bool ok = false;
    uint64_t o = inst_attr(v, idx, name, ok);
    return ok && inst_view(o, out);
}

// 返回 g_cam_ctrl.unit.pid、cam_handler.last_pitch、lmt_ver_view_range
static bool read_cam(uint64_t md, int &pid, float &lp, bool &spray, float &l0, float &l1, const char *&why)
{
    h_cam = PyCore::find_key(md, "g_cam_ctrl", h_cam);
    InstView cv, uv, hv;
    if (h_cam < 0 || !inst_view(PyCore::get_attr(md, h_cam), cv)) { why = "g_cam_ctrl"; return false; }
    pid = -1;
    if (attr_view(cv, h_unit, "unit", uv)) {
        bool ok = false; uint64_t p = inst_attr(uv, h_pid, "pid", ok); int64_t v = 0;
        if (ok && PyCore::read_int(p, v)) pid = (int)v;
    }
    if (!attr_view(cv, h_handler, "cam_handler", hv)) { why = "cam_handler"; return false; }
    bool ok = false;
    inst_attr(hv, h_lp, "last_pitch", ok);
    if (!ok || !PyCore::read_number(PyCore::inst_value_slot(hv, h_lp), lp, -90.0, 90.0)) { why = "last_pitch"; return false; }
    spray = false;
    uint64_t lmt = inst_attr(hv, h_lmt, "lmt_ver_view_range", ok);
    if (ok && type_is(lmt, "tuple")) {                         // 不喷射时是 None
        int64_t n = 0;
        if (vm_readv(lmt + 0x10, &n, 8) && n == 2) {          // tuple: +0x10 长度, +0x18 起元素
            float a, b;
            if (PyCore::read_number(lmt + 0x18, a, -90.0, 90.0) && PyCore::read_number(lmt + 0x20, b, -90.0, 90.0)) {
                l0 = fminf(a, b); l1 = fmaxf(a, b); spray = true;
            }
        }
    }
    return true;
}

static uint64_t read_ubt(uint64_t md)
{
    h_um = PyCore::find_key(md, "unit_mgr", h_um);
    InstView uv;
    if (h_um < 0 || !inst_view(PyCore::get_attr(md, h_um), uv)) return 0;
    bool ok = false;
    uint64_t ubt = inst_attr(uv, h_ubt, "units_by_type", ok);
    return ok ? ubt : 0;
}

static int read_scene_id(uint64_t md)
{
    h_wm = PyCore::find_key(md, "g_world_mgr", h_wm);
    InstView wv, cv;
    if (h_wm < 0 || !inst_view(PyCore::get_attr(md, h_wm), wv)) return -1;
    if (!attr_view(wv, h_cw, "cur_world", cv)) return -1;
    bool ok = false; uint64_t p = inst_attr(cv, h_sid, "scene_id", ok); int64_t v = -1;
    return (ok && PyCore::read_int(p, v)) ? (int)v : -1;
}

// 室内标记：units_by_type[3] 的 indoor_tag，按 model+0x20 配对
struct IndoorRec { uint64_t obj; bool indoor; };
static IndoorRec g_indoor[MAX_GEN]; static int g_nindoor = 0;
static void read_indoor(uint64_t ubt)
{
    uint64_t lst = PyCore::get_type_list(ubt, 3);
    int64_t n = 0; uint64_t items = 0;
    if (!PyCore::read_list(lst, n, items)) return;
    int c = 0;
    for (int64_t i = 0; i < n && c < MAX_GEN; i++) {
        InstView v;
        if (!inst_view(getPtr64(items + i * 8), v)) continue;
        bool ok = false;
        uint64_t mo = inst_attr(v, h_g_model, "model", ok);
        uint64_t so = ok ? PyCore::get_scene_obj(mo) : 0;
        if (!so) continue;
        bool indoor = false;
        inst_attr(v, h_g_indoor, "indoor_tag", ok);
        if (ok) PyCore::read_bool(PyCore::inst_value_slot(v, h_g_indoor), indoor);
        g_indoor[c].obj = so; g_indoor[c].indoor = indoor; c++;
    }
    g_nindoor = c;
}

// 求生者：units_by_type[2] 的场景坐标 + col_conf(半径, 高)
static void read_survivors(uint64_t ubt)
{
    g_nsurv = 0;
    uint64_t lst = PyCore::get_type_list(ubt, 2);
    int64_t n = 0; uint64_t items = 0;
    if (!PyCore::read_list(lst, n, items)) return;
    for (int64_t i = 0; i < n && g_nsurv < 8; i++) {
        InstView v;
        if (!inst_view(getPtr64(items + i * 8), v)) continue;
        bool ok = false;
        uint64_t mo = inst_attr(v, h_s_model, "model", ok);
        uint64_t so = ok ? PyCore::get_scene_obj(mo) : 0;
        float pos[3];
        uint64_t coor = so ? getPtr64(so + 0x28) : 0;
        if (!coor || !vm_readv(coor + 0xa0, pos, 12) || (pos[0] == 0 && pos[2] == 0) || pos[1] < -300) continue;
        Surv &s = g_surv[g_nsurv];
        memcpy(s.pos, pos, 12); s.r = 4.f; s.h = 16.f;
        uint64_t cc = inst_attr(v, h_s_col, "col_conf", ok);
        int64_t cn = 0;
        if (ok && is_obj(cc) && vm_readv(cc + 0x10, &cn, 8) && cn >= 2) {
            float r, h;
            if (PyCore::read_number(cc + 0x18, r, 0.5, 30.0) && PyCore::read_number(cc + 0x20, h, 1.0, 60.0)) { s.r = r; s.h = h; }
        }
        g_nsurv++;
    }
}

// ---------------- 主循环 ----------------
static uintptr_t g_libbase = 0;


// 面板显示用的状态(结构化，绘制线程按字段排版)
struct Status {
    int  pid = -1, scene = -1, n_surv = 0, nb = 0;
    bool read_ok = false, is_wax = false, spraying = false, self_ok = false;
    float last_pitch = 0, lmt0 = DEF_LMT_LO, lmt1 = DEF_LMT_HI;
    char why[64] = "";
};
static Status g_st;

// 收密码机(实时坐标 + 朝向取场景对象 3×3 的 Z 行)
static void collect_gens(Result &res)
{
    g_ngen = 0;
    for (int i = 0; i < PyProgress::gen_total() && g_ngen < MAX_GEN; i++) {
        PyProgress::Entry e;
        if (!PyProgress::get_gen_entry(i, e) || !e.scene_obj) continue;
        Gen &g = g_gen[g_ngen];
        uint64_t coor = getPtr64(e.scene_obj + 0x28);
        float m[12];
        if (!coor || !vm_readv(coor + 0x78, m, 48)) continue;      // +0x78 起 3×3(每行 12 字节)，+0xa0 位置
        float pos[3];
        if (!vm_readv(coor + 0xa0, pos, 12) || (pos[0] == 0 && pos[2] == 0)) continue;
        g.obj = e.scene_obj; memcpy(g.pos, pos, 12);
        float zx = m[6], zz = m[8], zl = sqrtf(zx * zx + zz * zz);  // +0x90/+0x98
        if (zl > 0.5f) { g.zx = zx / zl; g.zz = zz / zl; } else { g.zx = 0; g.zz = 1; }
        g.indoor = false;
        for (int k = 0; k < g_nindoor; k++) if (g_indoor[k].obj == g.obj) { g.indoor = g_indoor[k].indoor; break; }
        g.progress = e.progress;
        make_boxes(g);
        g_ngen++;
    }
    res.ng = g_ngen;
    for (int i = 0; i < g_ngen; i++) {
        res.gen_obj[i] = g_gen[i].obj; res.gen_green[i] = false;
    }
}

// 朝向：让"出生点右偏 2"之后的竖直面穿过目标点；ft = 目标在轨迹里的水平位置
static void aim_at(const float *O, const float *c, float &hx, float &hz, float &tl)
{
    float tx = c[0] - O[0], tz = c[2] - O[2]; tl = sqrtf(tx * tx + tz * tz);
    hx = tx / tl; hz = tz / tl;
    float sx = O[0] + hz * OFF_R, sz = O[2] - hx * OFF_R;
    tx = c[0] - sx; tz = c[2] - sz; tl = sqrtf(tx * tx + tz * tz);
    hx = tx / tl; hz = tz / tl;
}

// 天线区间：只认打中天线(kind 2)。先只测盒子(目标附近那几步)筛候选，候选再带场景从头跑。
// 带场景的完整轨迹贵，候选按步长 4 抽样，相邻两个抽样结果一样就把中间的照抄，不一样才逐个补跑
static unsigned char g_state[TBL_N];   // 0 打不中 1 能附着 2 能附着但穿过求生者；255 = 候选但还没跑
static void antenna_band(Result &res, const float *O, int gi, int i_lo, int i_hi, float dist_m)
{
    Gen &g = g_gen[gi];
    const Box &bx = g.box[1];
    float hx, hz, tl;
    aim_at(O, bx.c, hx, hz, tl);
    float ft = tl - OFF_F;
    float floor_h = fminf(O[1], g.pos[1]) - 1.f;
    auto full = [&](int pi) -> unsigned char {
        TraceOut o; trace(g_tbl[pi], O, hx, hz, gi, true, floor_h, o, false);
        return o.kind == 2 ? (o.surv ? 2 : 1) : 0;
    };
    int first = -1;
    for (int pi = i_lo; pi <= i_hi; pi++) {
        const Traj &tr = g_tbl[pi];
        int s_a = -1, s_b = -1;
        for (int s = 1; s < tr.n; s++) {
            if (tr.f[s] < ft - 30.f) continue;
            if (s_a < 0) s_a = s;
            s_b = s;
            if (tr.f[s] > ft + 30.f) break;
        }
        g_state[pi] = 0;
        if (s_a > 0) {
            TraceOut q; trace(tr, O, hx, hz, gi, false, -1e9f, q, false, s_a, s_b);
            if (q.kind == 2) { g_state[pi] = 255; if (first < 0) first = pi; }
        }
    }
    if (first < 0) return;
    // 抽样：每段候选的首尾 + 每隔 4 个跑完整轨迹；两个抽样点结果相同就照抄中间，不同就逐个补跑
    static bool cand[TBL_N];
    for (int pi = i_lo; pi <= i_hi; pi++) cand[pi] = g_state[pi] == 255;
    int last_eval = -1;
    for (int pi = first; pi <= i_hi; pi++) {
        if (!cand[pi]) { last_eval = -1; continue; }
        bool run_first = pi == i_lo || !cand[pi - 1];
        bool run_last  = pi == i_hi || !cand[pi + 1];
        if (!(run_first || run_last || pi - last_eval >= 4)) continue;
        unsigned char v = full(pi);
        if (last_eval >= 0 && pi - last_eval > 1) {
            unsigned char u = g_state[last_eval];
            for (int k = last_eval + 1; k < pi; k++) g_state[k] = (u == v) ? v : full(k);
        }
        g_state[pi] = v;
        last_eval = pi;
    }
    float half_yaw = atanf((R_UNIT + 5.f) / tl);
    int run = -1;
    for (int pi = i_lo; pi <= i_hi + 1; pi++) {
        unsigned char v = pi <= i_hi ? g_state[pi] : 0;
        if (v == 255) v = 0;                                     // 理论上不会剩
        if (run >= 0 && (v == 0 || v != g_state[run])) {
            if (res.nb < MAX_BANDS) {
                Band &b = res.b[res.nb++];
                b.hx = hx; b.hz = hz; b.half_yaw = half_yaw;
                b.p0 = TBL_LO + run * TBL_STEP - TBL_STEP * 0.5f; b.p1 = TBL_LO + (pi - 1) * TBL_STEP + TBL_STEP * 0.5f;
                b.dist_m = dist_m; b.part = 1; b.surv = g_state[run] == 2;
            }
            run = -1;
        }
        if (v != 0 && run < 0) run = pi;
    }
}

static void compute(Result &res, const float *O, float lp, bool spray, float l0, float l1)
{
    collect_gens(res);
    float cfx = g_cam_fx, cfz = g_cam_fz;
    float lo = spray ? l0 : DEF_LMT_LO, hi = spray ? l1 : DEF_LMT_HI;
    int i_lo = tbl_index(lo), i_hi = tbl_index(hi);

    for (int gi = 0; gi < g_ngen; gi++) {
        Gen &g = g_gen[gi];
        float dx = g.pos[0] - O[0], dz = g.pos[2] - O[2], dh = sqrtf(dx * dx + dz * dz);
        if (dh < 1 || dh > MAX_DIST || PyProgress::is_decoded(g.progress)) continue;   // 只管没破译完的
        float floor_h = fminf(O[1], g.pos[1]) - 1.f;
        // 距离变绿：最大仰角(俯仰下限)朝机身或天线能附着
        for (int part = 0; part < 2 && !res.gen_green[gi]; part++) {
            float hx, hz, tl;
            aim_at(O, g.box[part].c, hx, hz, tl);
            TraceOut o; trace(g_tbl[i_lo], O, hx, hz, gi, true, floor_h, o, false);
            if (o.kind == 1 || o.kind == 2) res.gen_green[gi] = true;
        }
        if (!(spray || show_always)) continue;
        if ((dx * cfx + dz * cfz) / dh < 0.5f) continue;            // 视野夹角 > 60°
        antenna_band(res, O, gi, i_lo, i_hi, dh / 11.886f);         // 机身区间预测不准(用户实测)，只画天线
    }

    // 落点预测：当前俯仰 + 相机水平朝向
    res.end_kind = 0;
    if (show_traj && spray) {
        TraceOut o; trace(g_tbl[tbl_index(lp)], O, cfx, cfz, -1, true, O[1] - 1.f - 200.f, o, false);
        res.end_kind = o.kind; memcpy(res.end, o.end, 12);
    }
}

static void thread_main()
{
    build_table();
    int tick = 0, scene = -2;
    while (true) {
        usleep(16 * 1000);                                          // 60 帧
        Result &res = g_res[1 - g_res_active.load()];
        res = Result();
        Status st;
        st.scene = scene;
        auto publish = [&]() { g_st = st; g_res_active = 1 - g_res_active.load(); };
        if (g_libbase == 0 || !PyRoot::ensure()) { snprintf(st.why, sizeof(st.why), "%s", T("Python 层未就绪", "Python layer not ready")); publish(); continue; }
        const char *why = "";
        uint64_t md = PyCore::game_kernel_dict(why);
        int pid = -1; float lp = 0, l0 = DEF_LMT_LO, l1 = DEF_LMT_HI; bool spray = false;
        if (!md || !read_cam(md, pid, lp, spray, l0, l1, why)) { snprintf(st.why, sizeof(st.why), "%s", why); publish(); continue; }
        st.read_ok = true; st.pid = pid;
        res.last_pitch = st.last_pitch = lp; res.spraying = st.spraying = spray;
        res.lmt0 = st.lmt0 = l0; res.lmt1 = st.lmt1 = l1;
        // pid 不论开没开都读：准星按角色记位置要用它。蜡像师功能关着时到这里就停
        res.is_wax = st.is_wax = enabled && (pid == WAX_PID) && PySelf::camp() == 1;
        if (!res.is_wax) { publish(); continue; }
        if (tick++ % 60 == 0) {                                     // 约 1 秒：scene_id / 室内标记
            int sid = read_scene_id(md);
            if (sid >= 0 && sid != scene) {
                scene = sid; load_col(sid);
            }
            st.scene = scene;
            uint64_t ubt = read_ubt(md);
            if (is_obj(ubt)) read_indoor(ubt);
        }
        {
            uint64_t ubt = read_ubt(md);
            if (is_obj(ubt)) read_survivors(ubt); else g_nsurv = 0;
        }
        st.n_surv = g_nsurv;
        uint64_t anchor = 0; float O[3];
        uint64_t coor = PySelf::anchor(anchor) ? getPtr64(anchor + 0x28) : 0;
        if (!coor || !vm_readv(coor + 0xa0, O, 12) || (O[0] == 0 && O[2] == 0)) {
            snprintf(st.why, sizeof(st.why), "%s", T("自身坐标读不到", "self position unreadable")); publish(); continue;
        }
        st.self_ok = true;
        compute(res, O, lp, spray, l0, l1);
        st.nb = res.nb;
        publish();
    }
}

static void start(uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase = libbase;
    std::thread(thread_main).detach();
}

static inline Status status() { return g_st; }
static inline const char *col_status() { return g_col_status[0] ? g_col_status : T("未加载", "not loaded"); }

static inline const Result &result() { return g_res[g_res_active.load()]; }

// 距离变绿：这台机在最大仰角(俯仰下限)能附着
static bool gen_green(uint64_t obj)
{
    const Result &r = result();
    if (!r.is_wax) return false;
    for (int i = 0; i < r.ng; i++) if (r.gen_obj[i] == obj) return r.gen_green[i];
    return false;
}

} // namespace WaxAim