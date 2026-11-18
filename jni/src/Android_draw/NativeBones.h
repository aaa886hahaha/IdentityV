#ifndef IDV_NATIVE_BONES_H
#define IDV_NATIVE_BONES_H
// ============================================================================
// 原生骨骼读取 —— 抄若辰 sub_3A6074/sub_39F0CC 的偏移链，走我们的驱动(getPtr64/vm_readv)，零注入。
// 完整链见 skill native-bones.md「2026-09-26 静态解完」节 + 「2026-10-18 局内活体」节。
//   M    = unit.model + 0x20 = 场景对象(= PySelf body/anchor 拿到的那个)
//   类型1(主路径,若辰先试这条)：
//     ANIM = [M+0x818] 非0；[ANIM+0x1D8] 非0(校验)；SKEL = [[ANIM+0x1E8]+0x30]
//     X26  = [M+0x28](= objcoor)；世界矩阵 = 64字节 @ X26+0x1C0(4x4 行主序,平移在第4行)
//     名字 = [SKEL+0x80]..[SKEL+0x88)，16字节/项 {u32 id, 池指针}；骨骼数 = (end-begin)/16
//            名字串 = Cstr @ ([[池 + (id>>14)*8] + (id&0x3FFF)*8] + 0x10)  ← 用每项自带池指针,绕开版本相关全局
//     POSE = [ANIM+0x248]；flag = u8[POSE+0x24]；ARRAY = [POSE+0x10 + ((flag&2)?8:0)]
//            每根 64 字节 4x4；骨骼 i 的 v = 第4行(x,y,z,w)，|w|>=0.001
//            世界坐标 = v × 世界矩阵(行向量右乘)
//   swap_yz：若辰按单位校准的标志(结果 y/z 要不要换)。我们拿自身 objcoor(+0xa0 = X/高度/Y)校准一次。
//   输出统一 (X, 高度, Y)，正好喂 draw_Gui 的投影(matrix[0]*X + matrix[4]*高度 + matrix[8]*Y)。
//
// 本模块只在渲染线程(Draw_Main)里调,单线程,g_namecache/g_swap_yz 无并发。show_draw_Bones 关时完全不执行。
// 失败分阶段写 g_log(面板/日志可看),第一次上机就知道链断在哪;真值对照用 probe2 的 get_bone_position(name)。
// ============================================================================
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <unordered_map>
#include "PySelf.h"     // 带出 getPtr64 / vm_readv 及驱动读原语

namespace NativeBones {

// 目标骨骼(连线端点)，slot 下标即顺序
static const char *const TARGET[] = {
    "biped head", "biped neck", "biped spine",
    "biped l clavicle", "biped l upperarm", "biped l forearm", "biped l hand",
    "biped r clavicle", "biped r upperarm", "biped r forearm", "biped r hand",
    "biped l thigh", "biped l calf", "biped l foot",
    "biped r thigh", "biped r calf", "biped r foot",
};
static const int NUM_TARGET = (int)(sizeof(TARGET) / sizeof(TARGET[0]));   // 17
// 连线(下标对 TARGET)，照抄若辰 DrawOverlay 的 16 条边
static const int EDGES[][2] = {
    {0,1},{1,2},{1,3},{3,4},{4,5},{5,6},{1,7},{7,8},{8,9},{9,10},
    {2,11},{11,12},{12,13},{2,14},{14,15},{15,16},
};
static const int NUM_EDGE = (int)(sizeof(EDGES) / sizeof(EDGES[0]));       // 16

struct Skeleton { float x[32], y[32], z[32]; bool ok[32]; bool valid; };

static inline bool is_ptr(uint64_t p) { return p > 0x5000000000ULL && p < 0x8000000000ULL; }

// begin/end/check_*：命中缓存时的校验依据。只按 SKEL 地址查的话，换局后新骨架落在旧地址上会沿用旧映射、骨架画乱(用户复现过)
struct NameMap { int idx[32]; int max_idx; uint64_t begin, end; int check_i; uint32_t check_id; };
static std::unordered_map<uint64_t, NameMap> g_namecache;   // 键 = SKEL
static std::atomic<int> g_swap_yz{-1};                      // -1 未定 / 0 不换 / 1 换
static char g_log[256] = "(骨骼未启用)";

static inline void setlog(const char *s) { snprintf(g_log, sizeof(g_log), "%s", s); }
static inline const char *log_text() { return g_log; }

// 一个骨骼名拆成 前缀("biped"/"biped01"/"biped03"...) + 目标后缀，返回前缀组号(0~MAX_GROUP-1)、
// 不是 "biped<两位数字> <目标>" 形式返回 -1。组 0 = 无前缀 "biped"，组 k = "biped%02d"(k 取数字+1，冲突时并到最后一组)
// 另有一组 BIP_GROUP = 3ds Max 默认命名 "bip001 head"/"bip001 l hand"…：疯眼 burke.skeleton 整套都是这种，
// 17 个目标全有(2026-11-05 离线普查 chr_boss 动画 NAME 块)，原来一个都认不出、骨骼不画
static const int MAX_GROUP = 9;
static const int BIP_GROUP = MAX_GROUP - 1;
static int split_biped(const char *name, int &target)
{
    if (strncmp(name, "bip", 3) == 0 && name[3] >= '0' && name[3] <= '9' && name[4] >= '0' && name[4] <= '9'
        && name[5] >= '0' && name[5] <= '9' && name[6] == ' ') {
        for (int t = 0; t < NUM_TARGET; t++)
            if (strcmp(name + 6, TARGET[t] + 5) == 0) { target = t; return BIP_GROUP; }
        return -1;
    }
    if (strncmp(name, "biped", 5) != 0) return -1;
    const char *p = name + 5;
    int group = 0;
    if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
        group = (p[0] - '0') * 10 + (p[1] - '0') + 1;
        if (group >= BIP_GROUP) group = BIP_GROUP - 1;   // biped 带编号的溢出并到最后一个 biped 组，不占 bip 组
        p += 2;
    }
    if (p[0] != ' ') return -1;
    for (int t = 0; t < NUM_TARGET; t++)
        if (strcmp(p, TARGET[t] + 5) == 0) { target = t; return group; }   // TARGET 都是 "biped xxx"，比 " xxx"
    return -1;
}

// 建 target 名字 -> 骨骼数组下标(每个 SKEL 只做一次)。
// 一个骨架里可能有好几套 biped：破轮 polun.skeleton 是三个人拼的，biped / biped01 只有头颈脊椎腿、没手臂，
// biped03 才 17 个目标全有(2026-10-30 从 chr_boss.idx 的动画 NAME 块读出)。所以按前缀分组，取命中最多的一组，
// 平手取无前缀那组 —— 普通角色只有 "biped"，结果跟原来一样。
// names_off = 名字表 begin 在 SKEL 里的偏移(老 0x80，end 在其后 +8)；str_off = 名字池节点里字符串的偏移(老 0x10)
static bool build_namemap(uint64_t SKEL, NameMap &nm, int names_off = 0x80, int str_off = 0x10)
{
    uint64_t begin = getPtr64(SKEL + names_off), end = getPtr64(SKEL + names_off + 8);
    if (!is_ptr(begin) || !is_ptr(end) || end <= begin) return false;
    uint64_t bytes = end - begin;
    if ((bytes & 15) != 0 || bytes > 0x2000) return false;
    int count = (int)(bytes / 16);
    if (count < 1 || count > 512) return false;
    int gidx[MAX_GROUP][NUM_TARGET];
    uint32_t gid[MAX_GROUP][NUM_TARGET];
    int ghits[MAX_GROUP] = {0};
    for (int g = 0; g < MAX_GROUP; g++) for (int t = 0; t < NUM_TARGET; t++) gidx[g][t] = -1;
    char name[136];
    for (int i = 0; i < count; i++) {
        uint64_t item = begin + (uint64_t)i * 16;
        uint32_t id = 0;
        if (!vm_readv(item, &id, 4)) continue;
        uint64_t pool = getPtr64(item + 8);
        if (!is_ptr(pool)) continue;
        uint64_t l1 = getPtr64(pool + (uint64_t)(id >> 14) * 8);
        if (!is_ptr(l1)) continue;
        uint64_t l2 = getPtr64(l1 + (uint64_t)(id & 0x3FFF) * 8);
        if (!is_ptr(l2)) continue;
        if (!vm_readv(l2 + str_off, name, sizeof(name) - 1)) continue;
        name[sizeof(name) - 1] = 0;
        int t = -1;
        int g = split_biped(name, t);
        if (g < 0 || gidx[g][t] >= 0) continue;
        gidx[g][t] = i; gid[g][t] = id; ghits[g]++;
        if (ghits[0] == NUM_TARGET) break;   // 无前缀组已满：平手也是它赢，后面不用再读(普通角色和原来一样提前结束)
    }
    int best = 0;
    for (int g = 1; g < MAX_GROUP; g++) if (ghits[g] > ghits[best]) best = g;
    nm.max_idx = -1;
    nm.begin = begin; nm.end = end; nm.check_i = -1; nm.check_id = 0;
    for (int t = 0; t < NUM_TARGET; t++) {
        int i = gidx[best][t];
        nm.idx[t] = i;
        if (i < 0) continue;
        if (i > nm.max_idx) nm.max_idx = i;
        if (nm.check_i < 0 || i < nm.check_i) { nm.check_i = i; nm.check_id = gid[best][t]; }   // 命中的骨骼里下标最小的当校验项
    }
    return nm.max_idx >= 0;
}

static int namemap_hits(const NameMap &nm)
{
    int h = 0;
    for (int t = 0; t < NUM_TARGET; t++) if (nm.idx[t] >= 0) h++;
    return h;
}

// POSE 当前帧的姿态 × 世界矩阵 -> 各目标骨骼的原始世界坐标(未做 swap_yz)。返回算出了几根；pose 双缓冲翻了返回 -1
// mode 0 = 老双缓冲 ARRAY=[POSE+0x10 / +0x18 按 flag(+0x24)&2]；mode 1 = ARRAY=[[POSE+0x10]+0x18](朋友写法的另一种读法)
static int pose_world(uint64_t POSE, const NameMap &nm, const float wm[16], float rx[], float ry[], float rz[], bool rok[], int mode = 0)
{
    for (int t = 0; t < NUM_TARGET; t++) rok[t] = false;
    uint8_t flag0 = 0;
    uint64_t ARRAY = 0;
    if (mode == 1) {
        uint64_t mid = getPtr64(POSE + 0x10);
        if (!is_ptr(mid)) return 0;
        ARRAY = getPtr64(mid + 0x18);
    } else {
        if (!vm_readv(POSE + 0x24, &flag0, 1)) return 0;
        ARRAY = getPtr64(POSE + 0x10 + ((flag0 & 2) ? 8 : 0));
    }
    if (!is_ptr(ARRAY)) return 0;
    int need = nm.max_idx + 1;
    if (need < 1 || need > 512) return 0;
    static thread_local float block[512 * 16 + 64];
    if (!vm_readv(ARRAY, block, (size_t)need * 64)) return 0;
    if (mode == 0) {
        uint8_t flag1 = 0;
        if (!vm_readv(POSE + 0x24, &flag1, 1)) return 0;
        if ((flag0 & 2) != (flag1 & 2)) return -1;                      // pose 翻了,整帧丢
    }
    int n = 0;
    for (int t = 0; t < NUM_TARGET; t++) {
        int bi = nm.idx[t];
        if (bi < 0 || bi > nm.max_idx) continue;
        const float *m = block + bi * 16;
        float v0 = m[12], v1 = m[13], v2 = m[14], v3 = m[15];          // 第4行
        if (!(std::isfinite(v0) && std::isfinite(v1) && std::isfinite(v2))) continue;
        if (fabsf(v3) < 0.001f) continue;
        float wx = v0*wm[0] + v1*wm[4] + v2*wm[8]  + v3*wm[12];
        float wy = v0*wm[1] + v1*wm[5] + v2*wm[9]  + v3*wm[13];
        float wz = v0*wm[2] + v1*wm[6] + v2*wm[10] + v3*wm[14];
        if (!(std::isfinite(wx) && std::isfinite(wy) && std::isfinite(wz))) continue;
        if (fabsf(wx) > 1e7f || fabsf(wy) > 1e7f || fabsf(wz) > 1e7f) continue;
        rx[t] = wx; ry[t] = wy; rz[t] = wz; rok[t] = true; n++;
    }
    return n;
}

// 结构反推用：这具骨架像不像长在位置 pos(X/高度/Y)的这个人身上 ——
// ≥10 根算得出、全部在水平 40 单位(约 3.4 米)内、高度在脚下 15 到头上 60 之间(y/z 两种摆法任一成立)，且不是全挤在一个点上
static bool skeleton_plausible(const float rx[], const float ry[], const float rz[], const bool rok[], const float pos[3])
{
    for (int swap = 0; swap < 2; swap++) {
        int n = 0; bool ok = true;
        float hmin = 1e9f, hmax = -1e9f;
        for (int t = 0; t < NUM_TARGET && ok; t++) {
            if (!rok[t]) continue;
            float h = swap ? rz[t] : ry[t], y2 = swap ? ry[t] : rz[t];
            if (fabsf(rx[t] - pos[0]) > 40 || fabsf(y2 - pos[2]) > 40 || h < pos[1] - 15 || h > pos[1] + 60) ok = false;
            if (h < hmin) hmin = h;
            if (h > hmax) hmax = h;
            n++;
        }
        if (ok && n >= 10 && hmax - hmin > 3) return true;
    }
    return false;
}

// 按当前骨骼链(SceneLayout::bone_layout)从 M 走到 SKEL，顺带拿到 POSE 的基节点。失败返回 false，why = 断在哪
static bool walk_bone_chain(uint64_t M, const SceneLayout::BoneLayout &bl, uint64_t &SKEL, uint64_t &POSE, const char *&why)
{
    uint64_t node[SceneLayout::MAX_HOP + 1];
    node[0] = M;
    for (int i = 0; i < bl.skel.n; i++) {
        node[i + 1] = getPtr64(node[i] + bl.skel.hop[i]);
        if (!is_ptr(node[i + 1])) { why = i == 0 ? "骨骼: ANIM(骨架链第1跳) 无效 —— 这版偏移不同?看日志[结构反推]骨骼" : "骨骼: 骨架链中途无效"; return false; }
    }
    if (SceneLayout::same_bone(bl, SceneLayout::BONE_DEFAULT) && !is_ptr(getPtr64(node[1] + 0x1D8))) { why = "骨骼: [ANIM+0x1D8] 校验失败"; return false; }
    SKEL = node[bl.skel.n];
    if (bl.pose_base < 0 || bl.pose_base >= bl.skel.n) { why = "骨骼: pose_base 越界"; return false; }
    POSE = getPtr64(node[bl.pose_base] + bl.pose_off);
    if (!is_ptr(POSE)) { why = "骨骼: POSE 无效"; return false; }
    return true;
}

// 读一个单位(M = model+0x20)的骨架。is_self=true 时顺便做 swap_yz 校准。只走类型1。
static bool read_unit(uint64_t M, bool is_self, Skeleton &out)
{
    out.valid = false;
    if (!is_ptr(M)) { if (is_self) setlog("骨骼: M 无效"); return false; }
    const SceneLayout::BoneLayout bl = SceneLayout::bone_layout();
    uint64_t SKEL = 0, POSE = 0;
    const char *why = "";
    if (!walk_bone_chain(M, bl, SKEL, POSE, why)) { if (is_self) setlog(why); return false; }
    const uint64_t ANIM = getPtr64(M + bl.skel.hop[0]);                 // 只给日志用
    // X26 = 等效 objcoor(老版 [M+0x28]；结构变了按反推出的坐标链走，见 SceneLayout.h)
    uint64_t X26 = SceneLayout::coor(M);
    if (!is_ptr(X26)) { if (is_self) setlog("骨骼: X26([M+0x28]) 无效"); return false; }
    float wm[16];
    if (!vm_readv(X26 + 0x1C0, wm, 64)) { if (is_self) setlog("骨骼: 世界矩阵读失败"); return false; }
    if (SceneLayout::coor(M) != X26) return false;                     // 防竞态
    for (int k = 0; k < 16; k++) if (!std::isfinite(wm[k])) return false;

    auto it = g_namecache.find(SKEL);
    if (it != g_namecache.end()) {                                     // 命中也要核对：名字表首尾指针 + 校验项的名字 id(2 次读)
        const NameMap &c = it->second;
        uint64_t be[2] = {0, 0};
        uint32_t cid = 0;
        bool same = vm_readv(SKEL + bl.names_off, be, 16)
                 && (be[0] & 0x00FFFFFFFFFFFFFFULL) == c.begin && (be[1] & 0x00FFFFFFFFFFFFFFULL) == c.end
                 && c.check_i >= 0 && vm_readv(c.begin + (uint64_t)c.check_i * 16, &cid, 4) && cid == c.check_id;
        if (!same) { g_namecache.erase(it); it = g_namecache.end(); }
    }
    if (it == g_namecache.end()) {
        NameMap nm;
        if (!build_namemap(SKEL, nm, bl.names_off, bl.str_off)) { if (is_self) setlog("骨骼: 名字表建不起来(名字池偏移?)"); return false; }
        if (g_namecache.size() > 32) g_namecache.clear();
        it = g_namecache.emplace(SKEL, nm).first;
    }
    const NameMap &nm = it->second;

    // 先把每根算成原始世界 (rx,ry,rz)
    float rx[32], ry[32], rz[32];
    bool rok[32];
    uint8_t flag0 = 0;
    vm_readv(POSE + 0x24, &flag0, 1);
    int got = pose_world(POSE, nm, wm, rx, ry, rz, rok, bl.pose_mode);
    if (got == 0 && is_self) setlog("骨骼: 姿态数组读不到(pose 双缓冲/ARRAY)");
    if (got <= 0) return false;

    // swap_yz 校准(自身,一次):比较根骨原始世界与 objcoor(+0xa0 = X/高度/Y)的水平两轴
    int swap = g_swap_yz.load();
    if (swap < 0 && is_self) {
        int pt = rok[2] ? 2 : -1;                                       // 优先 spine
        if (pt < 0) for (int t = 0; t < NUM_TARGET; t++) if (rok[t]) { pt = t; break; }
        float sc[3];
        if (pt >= 0 && vm_readv(X26 + 0xa0, sc, 12)) {
            // 不换: (X,高度,Y)=(rx,ry,rz) → 水平(rx,rz) 对 (sc0,sc2)
            // 换  : (X,高度,Y)=(rx,rz,ry) → 水平(rx,ry) 对 (sc0,sc2)
            float dn = fabsf(rx[pt] - sc[0]) + fabsf(rz[pt] - sc[2]);
            float ds = fabsf(rx[pt] - sc[0]) + fabsf(ry[pt] - sc[2]);
            swap = (ds < dn) ? 1 : 0;
            g_swap_yz.store(swap);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "骨骼就绪 ANIM=%llx SKEL=%llx maxidx=%d POSE=%llx flag=%d 根骨世界(%.1f,%.1f,%.1f) objcoor(%.1f,%.1f,%.1f) swap=%d",
                (unsigned long long)ANIM, (unsigned long long)SKEL, nm.max_idx,
                (unsigned long long)POSE, flag0, rx[pt], ry[pt], rz[pt], sc[0], sc[1], sc[2], swap);
            setlog(buf);   // 留给内部状态,不再打屏/打日志(已验证成功)
        }
    }
    if (swap < 0) swap = 0;                                             // 还没校准(非自身先用不换)

    for (int t = 0; t < NUM_TARGET; t++) {
        out.ok[t] = rok[t];
        if (!rok[t]) continue;
        out.x[t] = rx[t];
        if (swap == 1) { out.y[t] = rz[t]; out.z[t] = ry[t]; }          // 换 y/z
        else           { out.y[t] = ry[t]; out.z[t] = rz[t]; }
    }
    out.valid = true;
    return true;
}

} // namespace NativeBones
#endif // IDV_NATIVE_BONES_H
