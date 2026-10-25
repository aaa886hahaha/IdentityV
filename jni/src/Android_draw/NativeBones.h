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

// 建 target 名字 -> 骨骼数组下标(每个 SKEL 只做一次)
static bool build_namemap(uint64_t SKEL, NameMap &nm)
{
    uint64_t begin = getPtr64(SKEL + 0x80), end = getPtr64(SKEL + 0x88);
    if (!is_ptr(begin) || !is_ptr(end) || end <= begin) return false;
    uint64_t bytes = end - begin;
    if ((bytes & 15) != 0 || bytes > 0x2000) return false;
    int count = (int)(bytes / 16);
    if (count < 1 || count > 512) return false;
    for (int i = 0; i < NUM_TARGET; i++) nm.idx[i] = -1;
    nm.max_idx = -1;
    nm.begin = begin; nm.end = end; nm.check_i = -1; nm.check_id = 0;
    char name[136];
    int found = 0;
    for (int i = 0; i < count && found < NUM_TARGET; i++) {
        uint64_t item = begin + (uint64_t)i * 16;
        uint32_t id = 0;
        if (!vm_readv(item, &id, 4)) continue;
        uint64_t pool = getPtr64(item + 8);
        if (!is_ptr(pool)) continue;
        uint64_t l1 = getPtr64(pool + (uint64_t)(id >> 14) * 8);
        if (!is_ptr(l1)) continue;
        uint64_t l2 = getPtr64(l1 + (uint64_t)(id & 0x3FFF) * 8);
        if (!is_ptr(l2)) continue;
        if (!vm_readv(l2 + 0x10, name, sizeof(name) - 1)) continue;
        name[sizeof(name) - 1] = 0;
        for (int t = 0; t < NUM_TARGET; t++)
            if (nm.idx[t] < 0 && strcmp(name, TARGET[t]) == 0) {
                nm.idx[t] = i;
                if (i > nm.max_idx) nm.max_idx = i;
                if (nm.check_i < 0) { nm.check_i = i; nm.check_id = id; }   // 第一根命中的骨骼当校验项
                found++;
                break;
            }
    }
    return nm.max_idx >= 0;
}

// 读一个单位(M = model+0x20)的骨架。is_self=true 时顺便做 swap_yz 校准。只走类型1。
static bool read_unit(uint64_t M, bool is_self, Skeleton &out)
{
    out.valid = false;
    if (!is_ptr(M)) { if (is_self) setlog("骨骼: M 无效"); return false; }
    uint64_t ANIM = getPtr64(M + 0x818);
    if (!is_ptr(ANIM)) { if (is_self) setlog("骨骼: [M+0x818](ANIM) 无效 —— 可能是类型0/这版偏移不同"); return false; }
    if (!is_ptr(getPtr64(ANIM + 0x1D8))) { if (is_self) setlog("骨骼: [ANIM+0x1D8] 校验失败"); return false; }
    uint64_t P2 = getPtr64(ANIM + 0x1E8);
    uint64_t SKEL = is_ptr(P2) ? getPtr64(P2 + 0x30) : 0;
    if (!is_ptr(SKEL)) { if (is_self) setlog("骨骼: SKEL([[ANIM+0x1E8]+0x30]) 无效"); return false; }
    uint64_t X26 = getPtr64(M + 0x28);
    if (!is_ptr(X26)) { if (is_self) setlog("骨骼: X26([M+0x28]) 无效"); return false; }
    float wm[16];
    if (!vm_readv(X26 + 0x1C0, wm, 64)) { if (is_self) setlog("骨骼: 世界矩阵读失败"); return false; }
    if (getPtr64(M + 0x28) != X26) return false;                       // 防竞态
    for (int k = 0; k < 16; k++) if (!std::isfinite(wm[k])) return false;

    auto it = g_namecache.find(SKEL);
    if (it != g_namecache.end()) {                                     // 命中也要核对：名字表首尾指针 + 校验项的名字 id(2 次读)
        const NameMap &c = it->second;
        uint64_t be[2] = {0, 0};
        uint32_t cid = 0;
        bool same = vm_readv(SKEL + 0x80, be, 16)
                 && (be[0] & 0x00FFFFFFFFFFFFFFULL) == c.begin && (be[1] & 0x00FFFFFFFFFFFFFFULL) == c.end
                 && c.check_i >= 0 && vm_readv(c.begin + (uint64_t)c.check_i * 16, &cid, 4) && cid == c.check_id;
        if (!same) { g_namecache.erase(it); it = g_namecache.end(); }
    }
    if (it == g_namecache.end()) {
        NameMap nm;
        if (!build_namemap(SKEL, nm)) { if (is_self) setlog("骨骼: 名字表建不起来(名字池偏移?)"); return false; }
        if (g_namecache.size() > 32) g_namecache.clear();
        it = g_namecache.emplace(SKEL, nm).first;
    }
    const NameMap &nm = it->second;

    uint64_t POSE = getPtr64(ANIM + 0x248);
    if (!is_ptr(POSE)) { if (is_self) setlog("骨骼: POSE([ANIM+0x248]) 无效"); return false; }
    uint8_t flag0 = 0;
    if (!vm_readv(POSE + 0x24, &flag0, 1)) return false;
    uint64_t ARRAY = getPtr64(POSE + 0x10 + ((flag0 & 2) ? 8 : 0));
    if (!is_ptr(ARRAY)) { if (is_self) setlog("骨骼: ARRAY(pose双缓冲) 无效"); return false; }
    int need = nm.max_idx + 1;
    if (need < 1 || need > 512) return false;
    static thread_local float block[512 * 16];
    if (!vm_readv(ARRAY, block, (size_t)need * 64)) { if (is_self) setlog("骨骼: 姿态数组读失败"); return false; }
    uint8_t flag1 = 0;
    if (!vm_readv(POSE + 0x24, &flag1, 1)) return false;
    if ((flag0 & 2) != (flag1 & 2)) return false;                      // pose 翻了,整帧丢

    // 先把每根算成原始世界 (rx,ry,rz)
    float rx[32], ry[32], rz[32];
    bool rok[32];
    for (int t = 0; t < NUM_TARGET; t++) {
        rok[t] = false;
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
        rx[t] = wx; ry[t] = wy; rz[t] = wz; rok[t] = true;
    }

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
