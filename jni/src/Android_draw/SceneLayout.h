#pragma once
// ---- 场景对象结构(2026-11-15)：坐标链 / 类名链从写死改成"当前生效的链" ----
// 朋友机(更新的热更 so)：数组和签名都没变、本体照样在数组里，但类名链 +0xf8→+0x0→+0x8→+0x20→+0x20 读出的
// 长度无效、坐标 +0x28 是空的 —— 改的是对象结构。读线程进局后拿 Python 真值(unit.position / unit.model_path)
// 从本体场景对象出发在指针图里搜出新链(见 draw_Gui.cpp 的 layout_infer)，搜到就换到这里；
// 默认值就是 0917 版的老链，搜不到时行为跟以前完全一样。
//
// 坐标链：hop[0..n-1] 依次解引用，末端节点 +foff 起 12 字节 = X/高度/Y。
// 全部代码原来都按"objcoor = [obj+0x28]，objcoor+0xa0 = 位置"写(+0x78 旋转、+0x1C0 世界矩阵也相对它)，
// 所以 coor() 返回"等效 objcoor" = 末端节点 + foff - 0xa0，各处 +0xa0/+0x78/+0x1C0 一行不用改。
// 这等于假设整块坐标结构是一起搬家的；搜索时会顺手核对 +0x1C0 矩阵平移是否也对得上(日志「整体」列)。
//
// 类名链：hop[0..n-1] 依次解引用到末端节点，节点 +sptr = 字符串指针、+sptr+8 = 长度(u32)。
// 老链 hop={0xf8,0,8,0x20,0x20} sptr=8。读线程按第一跳的值做类名缓存校验，换链时清缓存。

#include <cstdint>
#include <atomic>

namespace SceneLayout {

static const int MAX_HOP = 6;
struct Chain {
    int     n;                // 跳数
    int16_t hop[MAX_HOP];     // 每跳读指针的偏移
    int16_t fin;              // 坐标链：末端节点里 X 的偏移；类名链：末端节点里字符串指针的偏移
};

static const Chain COOR_DEFAULT = {1, {0x28}, 0xa0};
static const Chain NAME_DEFAULT = {5, {0xf8, 0x0, 0x8, 0x20, 0x20}, 0x8};

// 双份 + 原子下标：读线程换链，渲染线程/各 Py 线程同时在读
static Chain g_coor[2] = {COOR_DEFAULT, COOR_DEFAULT};
static Chain g_name[2] = {NAME_DEFAULT, NAME_DEFAULT};
static std::atomic<int> g_coor_active{0}, g_name_active{0};
static std::atomic<uint32_t> g_name_gen{0};     // 类名链每换一次 +1(读线程据此清类名缓存)

static inline bool ptr_ok(uint64_t p) { return p > 0x5000000000ULL && p < 0x8000000000ULL; }
static inline bool same(const Chain &a, const Chain &b)
{
    if (a.n != b.n || a.fin != b.fin) return false;
    for (int i = 0; i < a.n; i++) if (a.hop[i] != b.hop[i]) return false;
    return true;
}

static inline Chain coor_chain() { return g_coor[g_coor_active.load()]; }
static inline Chain name_chain() { return g_name[g_name_active.load()]; }
static inline bool coor_is_default() { return same(coor_chain(), COOR_DEFAULT); }

static inline void set_coor(const Chain &c) { int w = 1 - g_coor_active.load(); g_coor[w] = c; g_coor_active = w; }
static inline void set_name(const Chain &c) { int w = 1 - g_name_active.load(); g_name[w] = c; g_name_active = w; g_name_gen++; }

// 等效 objcoor(见文件头)。读不到返回 0，调用方原来对 getPtr64(obj+0x28)==0 的处理照旧生效
static inline uint64_t coor(uint64_t obj)
{
    if (!obj) return 0;
    const Chain c = coor_chain();
    if (c.n == 1 && c.hop[0] == 0x28 && c.fin == 0xa0) return getPtr64(obj + 0x28);   // 老链：跟原来一模一样
    uint64_t p = obj;
    for (int i = 0; i < c.n; i++) {
        p = getPtr64(p + c.hop[i]);
        if (!ptr_ok(p)) return 0;
    }
    return p + c.fin - 0xa0;
}

// 把类名链的第 1 跳之后走完，返回末端节点(读不到返回 0)。head = 第 1 跳读出的值
static inline uint64_t name_node(uint64_t head, const Chain &c)
{
    uint64_t p = head;
    for (int i = 1; i < c.n; i++) {
        if (!ptr_ok(p)) return 0;
        p = getPtr64(p + c.hop[i]);
    }
    return ptr_ok(p) ? p : 0;
}

// ---- 骨骼链(2026-11-15)：老版 SKEL = [[[M+0x818]+0x1E8]+0x30]，POSE = [ANIM+0x248](ANIM = 骨架链第 1 跳的节点) ----
// skel = 从场景对象 M 到 SKEL 的整条链(fin 不用)；POSE = [链上第 pose_base 个节点 + pose_off](0 = M 本身)。
// SKEL/POSE 内部布局(名字表 +0x80/+0x88、flag +0x24、双缓冲 +0x10/+0x18)假设没变，反推时就是按它们找的
// 2026-11-18：朋友机这版 SKEL 内部也动了 —— 名字表 +0x80→+0x88(朋友自己找到的完整链：ANIM=[M+0x860]、SKEL=[[ANIM+0x1e8]+0x30]、
// 名字表 [SKEL+0x88])，所以 SKEL 内部偏移也做成可换：names_off = 名字表 begin 的偏移(end 紧跟其后 +8)，
// str_off = 名字池节点里字符串的偏移(老 +0x10)，pose_mode 0 = 老双缓冲 [POSE+0x10/+0x18 按 flag]，1 = [[POSE+0x10]+0x18]
struct BoneLayout { Chain skel; int pose_base; int16_t pose_off; int16_t names_off; int16_t str_off; int8_t pose_mode; };
static const BoneLayout BONE_DEFAULT = {{3, {0x818, 0x1E8, 0x30}, 0}, 1, 0x248, 0x80, 0x10, 0};
// 已知版本表：反推时先逐个核对，对上就直接用(比搜索快、也不怕搜索假设不全)；没对上再搜
static const BoneLayout BONE_KNOWN[] = {
    BONE_DEFAULT,                                                    // 0917(用户本机)
    {{3, {0x860, 0x1E8, 0x30}, 0}, 1, 0x248, 0x88, 0x10, 0},         // 2026-11 朋友机(朋友手找)
    {{3, {0x860, 0x1E8, 0x30}, 0}, 1, 0x248, 0x88, 0x10, 1},         //   同上，POSE 按"+0x10+0x18 是两跳"理解
};
static const int NUM_BONE_KNOWN = (int)(sizeof(BONE_KNOWN) / sizeof(BONE_KNOWN[0]));
static BoneLayout g_bone[2] = {BONE_DEFAULT, BONE_DEFAULT};
static std::atomic<int> g_bone_active{0};
static inline BoneLayout bone_layout() { return g_bone[g_bone_active.load()]; }
static inline bool same_bone(const BoneLayout &a, const BoneLayout &b)
{
    return same(a.skel, b.skel) && a.pose_base == b.pose_base && a.pose_off == b.pose_off
        && a.names_off == b.names_off && a.str_off == b.str_off && a.pose_mode == b.pose_mode;
}
static inline bool bone_is_default() { return same_bone(bone_layout(), BONE_DEFAULT); }
static inline void set_bone(const BoneLayout &b) { int w = 1 - g_bone_active.load(); g_bone[w] = b; g_bone_active = w; }

// 日志用："+0xf8→+0x0→… 末端+0x8"
static inline void format(const Chain &c, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < c.n && k + 12 < cap; i++)
        k += snprintf(out + k, cap - k, "%s+0x%x", i ? "→" : "", (unsigned)c.hop[i]);
    if (k + 16 < cap) snprintf(out + k, cap - k, " 末端+0x%x", (unsigned)c.fin);
}

} // namespace SceneLayout
