#pragma once
// ============================================================================
// PyCore.h —— 读游戏内嵌 CPython 对象图的公共工具，PyProgress / PyGenius / PySelf 共用
// ============================================================================
//
// 2026-09-23 之前这些函数在三个模块里各抄了一份(每份约 150 行)，差别只有值域上下界。
// 代价是实测过的：给 find_key 加"整块读 + 驻留键指针"那次优化，要往三个文件里贴三遍同样的代码，
// 改 bug 同理。现在统一放这里，优化和修复一次生效。
//
// 解释器版本是 CPython 3.11，结构布局说明见 PyProgress.h 文件头，这里只放实现：
//   PyASCIIObject: 长度 +0x10，紧凑 ASCII 数据 +0x30
//   PyDictObject : ma_keys +0x20；keys 头 +0x09=dk_log2_index_bytes +0x0A=dk_kind +0x18=dk_nentries
//                  entries = keys + 32 + (1<<log2_index_bytes)，步长 16(全 unicode) / 24(带 hash)
//   managed-dict 实例: dict 指针在对象前 0x18
//   PyLongObject : +0x10 ob_size, +0x18 第一个 digit
//   list         : +0x10 ob_size, +0x18 ob_item
//
// 类型对象和 True/False 一律取自 PyRoot(运行时找出来的)，这里不再各存一份。
//
// ！！线程安全 ！！
// 三个模块各跑一个线程(16 / 100 / 500ms)，会同时调用这里的函数。所以带状态的两样东西
// —— 整块读缓冲 g_entry_block 和驻留键指针表 —— 必须是 thread_local，否则三个线程互相踩。
// 合并前它们分属三个命名空间、天然隔离，合并时如果照搬成普通 static 就会引入真实的数据竞争。
// 其余函数都是纯函数(只读目标进程内存)，无状态。

#include <cstdint>
#include <cstring>
#include <cstdio>
#include "PyRoot.h"

namespace PyCore {

// 引擎/CPython 指针高位常年是 0xB4(Android TBI)，getPtr64 内部已经掩过，所以这里比较的都是掩码后的值。
// 下限必须跟场景层(draw_Gui.cpp is_user_ptr)一致用 0x50：本机指针虽然都在 0x77xx~0x7Cxx，
// 但 so/堆落在哪取决于地址随机化和进程映射总量，别的设备/渠道包(多带一堆 SDK)会落到 0x6x 段。
// 原来卡 0x70 时，2026-09-24 渠道服反馈整个 Python 层(自身/进度/天赋)全灭、场景层正常。
// 挡垃圾不靠这个范围：每一步都有类型指针/结构校验
inline bool is_obj(uint64_t p) { return p > 0x5000000000ULL && p < 0x8000000000ULL; }

inline bool str_equals(uint64_t s, const char *want)
{
    if (!is_obj(s)) return false;
    int64_t len = 0;
    if (!vm_readv(s + 0x10, &len, 8)) return false;
    size_t n = strlen(want);
    if (len != (int64_t)n || n >= 64) return false;
    char buf[64];
    if (!vm_readv(s + 0x30, buf, n)) return false;
    return memcmp(buf, want, n) == 0;
}

// 读一个紧凑 ASCII 的 PyUnicode(账号 uid 这类纯数字串就是)。
// +0x10 长度，+0x20 的低位字节是 state 位域：compact ascii 时数据紧跟在 +0x30。
// 非 ASCII(中文名字是 UCS2/4)一律返回 false —— 要显示中文得另做解码，这里不猜
inline bool read_str(uint64_t s, char *out, size_t cap)
{
    if (!is_obj(s) || cap == 0) return false;
    int64_t len = 0;
    if (!vm_readv(s + 0x10, &len, 8)) return false;
    if (len <= 0 || (size_t)len >= cap) return false;
    uint32_t state = 0;
    if (!vm_readv(s + 0x20, &state, 4)) return false;
    if (!((state >> 6) & 1)) return false;            // compact ascii 标志位
    if (!vm_readv(s + 0x30, out, (size_t)len)) return false;
    out[len] = 0;
    return true;
}

struct Dict { uint64_t entries; int stride; int64_t n_entries; };

// ⚠ entries 起始必须逐实例算：同一个类的不同实例 dict 大小可以不一样，
// 把一个实例算出的偏移套到别的实例上会读到无关属性
inline bool read_dict(uint64_t d, Dict &out)
{
    if (!is_obj(d)) return false;
    uint64_t keys = getPtr64(d + 0x20);
    if (!is_obj(keys)) return false;
    uint8_t hdr[32];
    if (!vm_readv(keys, hdr, 32)) return false;
    uint8_t idxb = hdr[9];
    uint8_t kind = hdr[10];                       // 0=通用(带hash,24字节) 其它=全unicode(16字节)
    int64_t nent = 0; memcpy(&nent, hdr + 24, 8);
    if (idxb > 30 || nent < 0 || nent > (1 << 22)) return false;
    out.entries   = keys + 32 + ((uint64_t)1 << idxb);
    out.stride    = (kind == 0) ? 24 : 16;
    out.n_entries = nent;
    return true;
}

inline uint64_t key_slot(const Dict &k, int64_t i)   { return k.entries + i * k.stride + (k.stride == 24 ? 8 : 0); }
inline uint64_t value_slot(const Dict &k, int64_t i) { return k.entries + i * k.stride + (k.stride == 24 ? 16 : 8); }

// ---- 字典查找的两项优化(2026-09-23) ----
// 原来每比一个键要 3 次驱动读(取键指针 + 读长度 + 读内容)，全扫上千条的实例字典就是几千次读。
//   a) 整块读条目：entries 是连续内存，一次 vm_readv 读完(1300 条 * 24 字节 = 31KB)
//   b) 驻留键指针：CPython 会驻留标识符形式的属性名，同一个属性名在进程里就是同一个字符串对象。
//      第一次按名字找到后记下键对象地址，以后只比指针，连字符串都不用读。
//      假设不成立时自动退回按名字比较，只是慢一点，不会读错。
// 效果：命中缓存的校验从 3 次读降到 1 次；全扫从几千次降到 1 次(整块) + 本地比较。
// 两者都是 thread_local，见文件头「线程安全」。
inline thread_local uint8_t g_entry_block[48 * 1024];
struct InternedName { const char *name; uint64_t key_obj; };
inline thread_local InternedName g_interned[32];
inline thread_local int g_interned_n = 0;

inline uint64_t interned_of(const char *name)
{
    for (int i = 0; i < g_interned_n; i++)
        if (strcmp(g_interned[i].name, name) == 0) return g_interned[i].key_obj;
    return 0;
}

inline void remember_interned(const char *name, uint64_t kp)
{
    for (int i = 0; i < g_interned_n; i++)
        if (strcmp(g_interned[i].name, name) == 0) { g_interned[i].key_obj = kp; return; }
    if (g_interned_n < (int)(sizeof(g_interned) / sizeof(g_interned[0]))) {
        g_interned[g_interned_n].name = name;      // 调用方传的都是字符串常量，生命周期同进程
        g_interned[g_interned_n].key_obj = kp;
        g_interned_n++;
    }
}

inline int64_t find_key(uint64_t d, const char *name, int64_t hint)
{
    Dict k;
    if (!read_dict(d, k)) return -1;
    const uint64_t want = interned_of(name);
    if (hint >= 0 && hint < k.n_entries) {
        uint64_t kp = getPtr64(key_slot(k, hint));
        if (want && kp == want) return hint;                        // 命中缓存：只花 1 次读
        if (str_equals(kp, name)) { remember_interned(name, kp); return hint; }
    }
    size_t bytes = (size_t)k.n_entries * k.stride;
    int key_off = (k.stride == 24) ? 8 : 0;
    if (bytes > 0 && bytes <= sizeof(g_entry_block) && vm_readv(k.entries, g_entry_block, bytes)) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0 && !want) continue;                       // 还不知道键地址，直接进第二遍
            for (int64_t i = 0; i < k.n_entries; i++) {
                uint64_t kp = 0;
                memcpy(&kp, g_entry_block + (size_t)i * k.stride + key_off, 8);
                kp &= 0x00FFFFFFFFFFFFFFULL;
                if (pass == 0) { if (kp == want) return i; }
                else if (str_equals(kp, name)) { remember_interned(name, kp); return i; }
            }
        }
        return -1;
    }
    // 整块读失败(条目跨到换出的页等)：退回逐条读
    for (int64_t i = 0; i < k.n_entries; i++)
        if (str_equals(getPtr64(key_slot(k, i)), name)) return i;
    return -1;
}

inline uint64_t get_attr(uint64_t d, int64_t idx)
{
    Dict k;
    if (!read_dict(d, k) || idx < 0 || idx >= k.n_entries) return 0;
    return getPtr64(value_slot(k, idx));
}

// 某个属性的序号：先用缓存的并校验，不对就全扫重找。序号随类属性插入顺序变，热更后可能位移，
// 所以从不直接信任缓存 —— 最坏就是多扫一遍，不会读错属性
inline int64_t resolve_index(uint64_t d, const Dict &dk, int64_t cached, const char *name)
{
    if (cached >= 0 && cached < dk.n_entries) {
        uint64_t kp = getPtr64(key_slot(dk, cached));
        uint64_t want = interned_of(name);
        if (want && kp == want) return cached;                      // 驻留键指针：校验只要 1 次读
        if (str_equals(kp, name)) { remember_interned(name, kp); return cached; }
    }
    return find_key(d, name, -1);
}

// managed-dict 实例：dict 指针在对象前 0x18。多校验一次 ob_type 是 dict，避免把别的东西当实例
inline uint64_t get_inst_dict(uint64_t obj)
{
    if (!is_obj(obj)) return 0;
    uint64_t d = getPtr64(obj - 0x18);
    if (!is_obj(d) || getPtr64(d + 8) != PyRoot::g_dict_type) return 0;
    return d;
}

// 读一个 PyLong。带防竞态校验：解引用后重读指针确认没变，再看 refcount 和类型
inline bool read_int(uint64_t vp, int64_t &out)
{
    if (!is_obj(vp)) return false;
    uint8_t b[32];
    if (!vm_readv(vp, b, 32)) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
    if (rc <= 0 || tp != PyRoot::g_int_type) return false;
    int64_t sz = 0; memcpy(&sz, b + 16, 8);
    uint32_t dg = 0; memcpy(&dg, b + 24, 4);
    out = (sz == 0) ? 0 : (int64_t)dg;
    if (sz < 0) out = -out;
    return true;
}

// 读一个 int/float 属性到 float。lo/hi 是值域闸：进度是 0~100，冷却是 0~600，**不能混用**。
// 三次重试 + 指针重读是必须的：属性每帧被赋成新的 float 对象，旧的进 freelist 被复用
inline bool read_number(uint64_t slot, float &out, double lo, double hi)
{
    for (int t = 0; t < 3; t++) {
        uint64_t vp = getPtr64(slot);
        if (!is_obj(vp)) continue;
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) continue;
        if (getPtr64(slot) != vp) continue;       // 解引用期间指针变了 -> 对象可能已被释放复用
        int64_t rc = 0; memcpy(&rc, b, 8);
        uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
        if (rc <= 0) continue;                    // refcount 0 = 已释放
        double v;
        if (tp == PyRoot::g_float_type) {
            memcpy(&v, b + 16, 8);
        } else if (tp == PyRoot::g_int_type) {    // 0 和 100 常是缓存的小整数对象，必须两种都认
            int64_t sz = 0; memcpy(&sz, b + 16, 8);
            uint32_t dg = 0; memcpy(&dg, b + 24, 4);
            v = (sz == 0) ? 0.0 : (double)dg;
            if (sz < 0) v = -v;
        } else {
            continue;
        }
        if (!(v >= lo && v <= hi)) continue;      // 值域闸：垃圾值里出现过 0.16 这种"看着合理"的
        out = (float)v;
        return true;
    }
    return false;
}

// 读一个 bool。先比 PyRoot 找出来的两个单例(最稳)，比不上再按 PyLong 布局退化解
inline bool read_bool(uint64_t slot, bool &out)
{
    uint64_t vp = getPtr64(slot);
    if (!is_obj(vp)) return false;
    if (vp == PyRoot::g_true)  { out = true;  return true; }
    if (vp == PyRoot::g_false) { out = false; return true; }
    uint8_t b[32];
    if (!vm_readv(vp, b, 32)) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    if (rc <= 0) return false;
    int64_t sz = 0; memcpy(&sz, b + 16, 8);
    uint32_t dg = 0; memcpy(&dg, b + 24, 4);
    out = (sz != 0 && dg != 0);
    return true;
}

// list: +0x10 元素数, +0x18 元素数组。
// 三点语义要守住(合并前三个模块都是这样，改了会连累天赋记忆和本体集合)：
//   1) n > 256 视为读到垃圾 -> 失败。上限不能放宽：调用方会按 n 逐个读元素，
//      n 是垃圾大数时会让 16ms 那个线程空转几千次读
//   2) **空列表是成功**(n=0, items=0)，不是失败 —— "读不到"和"真的是空"必须分开，
//      这条是天赋那边踩过的坑(见 PyGenius.h 文件头第 3 条)
//   3) 非空时 items 必须是合法堆指针
inline bool read_list(uint64_t L, int64_t &n, uint64_t &items)
{
    if (!is_obj(L)) return false;
    if (!vm_readv(L + 0x10, &n, 8)) return false;
    if (n < 0 || n > 256) return false;
    if (n == 0) { items = 0; return true; }
    items = getPtr64(L + 0x18);
    return is_obj(items);
}

// units_by_type 是 dict(int -> list)，按 int 键取出那条 list
inline uint64_t get_type_list(uint64_t ubt, int type)
{
    Dict k;
    if (!read_dict(ubt, k)) return 0;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (!is_obj(kp) || getPtr64(kp + 8) != PyRoot::g_int_type) continue;
        int64_t sz = 0; uint32_t dg = 0;
        vm_readv(kp + 16, &sz, 8);
        vm_readv(kp + 24, &dg, 4);
        if (sz == 1 && (int)dg == type) return getPtr64(value_slot(k, i));
    }
    return 0;
}

// sys.modules -> "game_kernel" -> md_dict(进程内稳定，不随对局变)。
// 失败原因写进 why，调用方直接拿去显示在面板上
inline uint64_t game_kernel_dict(const char *&why)
{
    uint64_t sm = getPtr64(PyRoot::g_modules_slot);
    if (!is_obj(sm))                          { why = T("sys.modules 取不到", "sys.modules unreadable"); return 0; }
    if (getPtr64(sm + 8) != PyRoot::g_dict_type) { why = T("sys.modules 不是dict(偏移已失效)", "sys.modules is not a dict (offset stale)"); return 0; }
    int64_t i = find_key(sm, "game_kernel", -1);
    if (i < 0)                                { why = T("找不到 game_kernel 模块", "game_kernel module not found"); return 0; }
    uint64_t mod = get_attr(sm, i);
    if (!is_obj(mod))                         { why = T("game_kernel 模块对象无效", "game_kernel module object invalid"); return 0; }
    uint64_t md = getPtr64(mod + 0x10);
    if (!is_obj(md))                          { why = T("md_dict 无效", "md_dict invalid"); return 0; }
    return md;
}

// unit.model -> +0x20 = 场景对象指针(引擎自己持有的身份，用来跟绘制循环里的 obj 配对)
inline uint64_t get_scene_obj(uint64_t model_obj)
{
    if (!is_obj(model_obj)) return 0;
    uint64_t sp = getPtr64(model_obj + 0x20);
    return is_obj(sp) ? sp : 0;
}

// ---- 游戏阶段(大厅/准备/局内)判定 ----
// 判据 = g_ui_mgr.ui_dict 里挂着哪个 UI 对象，纯对象存在性，不用 world_type(用户已否决:多个大厅/地图下它各不同)。
// 2026-10-15 跨进程实测(py_scene.py)并与 09-24 注入样本一致:
//   大厅 = {UIHall}   准备 = {UIRoomWait}   局内 = {UIBattle, UIControlCustom}
// 三态 key 完全不重叠。局内有 2 个 key，所以查"某 key 是否存在"、按 局内->准备->大厅 顺序，
// reason 回命中的那个 key(= 007 domain 的 selection_reason)。走链见 idv-reverse-resume SKILL「区分三个阶段」。
// 与 units_by_type 键 1/2(uses_battle_world)互为交叉校验:UIBattle 在 == 有键 1/2。
enum Phase { PHASE_UNKNOWN = 0, PHASE_LOBBY = 1, PHASE_PREPARE = 2, PHASE_BATTLE = 3 };

inline Phase detect_phase(const char *&reason)
{
    // g_ui_mgr 在模块 dict、ui_dict 在 UIManager 实例 dict，两个条目序号进程内稳定、跨对局不变。
    // thread_local 配合 PyScene 单线程调用(见 PyCore 文件头「线程安全」)。find_key 自带校验，缓存错了会重扫。
    static thread_local int64_t hint_uimgr = -1, hint_uidict = -1;
    const char *why = "";
    uint64_t md = game_kernel_dict(why);
    if (!md) { reason = why; return PHASE_UNKNOWN; }

    int64_t iu = find_key(md, "g_ui_mgr", hint_uimgr);
    if (iu < 0) { reason = T("没有 g_ui_mgr", "no g_ui_mgr"); return PHASE_UNKNOWN; }
    hint_uimgr = iu;
    uint64_t ud = get_inst_dict(get_attr(md, iu));
    if (!ud) { reason = T("g_ui_mgr 实例 dict 无效", "g_ui_mgr instance dict invalid"); return PHASE_UNKNOWN; }

    int64_t id = find_key(ud, "ui_dict", hint_uidict);
    if (id < 0) { reason = T("没有 ui_dict", "no ui_dict"); return PHASE_UNKNOWN; }
    hint_uidict = id;
    uint64_t uidict = get_attr(ud, id);

    Dict k;
    if (!read_dict(uidict, k)) { reason = T("ui_dict 读取失败", "ui_dict read failed"); return PHASE_UNKNOWN; }
    bool has_room = false, has_hall = false;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (!is_obj(kp)) continue;
        if (str_equals(kp, "UIBattle")) { reason = "UIBattle"; return PHASE_BATTLE; }
        else if (str_equals(kp, "UIRoomWait")) has_room = true;
        else if (str_equals(kp, "UIHall")) has_hall = true;
    }
    if (has_room) { reason = "UIRoomWait"; return PHASE_PREPARE; }
    if (has_hall) { reason = "UIHall"; return PHASE_LOBBY; }
    reason = T("无已知 UI key", "no known UI key");
    return PHASE_UNKNOWN;
}

// ---- 主交互按钮：游戏自己算的"现在这个按钮会做什么、对谁做"(2026-10-23 静态分析，未上机) ----
// UIBattleButton 是 UIBattleBase 的父类(混入)，所以属性直接挂在 ui_dict['UIBattle'] 实例上：
//   cbtn_alpha_st  CIV_HAND_ALPHA_STATE。2026-10-23 国际服上机日志实测：**3 = PUT_PANEL(放板)、4 = SPAN_PANEL(翻板，放下后立刻变 4)、
//                  10 = 空闲**。其余(SP_FLAG 1 / CREEP 2 / SPAN 5 / CAVE 6 / SAVE_TGT 7 …)是按 consts/const 定义顺序推的，未实测
//   cbtn_tgt_uid   目标单位 uid(放板时 = 板子的 uid)
// 依据：ui/UIBattlePart/UIBattleButton.tick_btn_alpha 每帧写这两个属性；core/key_handlers/pc_hotkey 按键时直接读它们。
// 是否能放板由 BattlePlayerUnit.find_putdown_panel_uid 决定(三维距离 ≤ wood_put_dis 25 + 加成、板立着、无遮挡、单向板朝向…)，
// 读这个结果就不用自己复刻那一串条件。线程：自动盖板线程独占调用(thread_local 缓存)。
struct InterButton {
    int64_t alpha_st = -1;      // -1 = 没读到
    int64_t tgt_uid  = -1;      // -1 = 没读到 / 不是 int(None 等)
};
inline bool read_inter_button(InterButton &out, const char *&why)
{
    static thread_local int64_t h_uimgr = -1, h_uidict = -1, h_battle = -1, h_st = -1, h_uid = -1;
    out = InterButton();
    uint64_t md = game_kernel_dict(why);
    if (!md) return false;
    int64_t iu = find_key(md, "g_ui_mgr", h_uimgr);
    if (iu < 0) { why = T("没有 g_ui_mgr", "no g_ui_mgr"); return false; }
    h_uimgr = iu;
    uint64_t ud = get_inst_dict(get_attr(md, iu));
    if (!ud) { why = T("g_ui_mgr 实例 dict 无效", "g_ui_mgr instance dict invalid"); return false; }
    int64_t id = find_key(ud, "ui_dict", h_uidict);
    if (id < 0) { why = T("没有 ui_dict", "no ui_dict"); return false; }
    h_uidict = id;
    uint64_t uidict = get_attr(ud, id);
    int64_t ib = find_key(uidict, "UIBattle", h_battle);
    if (ib < 0) { why = T("没有 UIBattle(不在局内)", "no UIBattle (not in match)"); return false; }
    h_battle = ib;
    uint64_t bd = get_inst_dict(get_attr(uidict, ib));
    if (!bd) { why = T("UIBattle 实例 dict 无效", "UIBattle instance dict invalid"); return false; }
    int64_t is = find_key(bd, "cbtn_alpha_st", h_st);
    if (is < 0) { why = T("UIBattle 上没有 cbtn_alpha_st", "no cbtn_alpha_st on UIBattle"); return false; }
    h_st = is;
    if (!read_int(get_attr(bd, is), out.alpha_st)) { out.alpha_st = -1; why = T("cbtn_alpha_st 不是 int", "cbtn_alpha_st is not int"); return false; }
    int64_t it = find_key(bd, "cbtn_tgt_uid", h_uid);
    if (it >= 0) { h_uid = it; if (!read_int(get_attr(bd, it), out.tgt_uid)) out.tgt_uid = -1; }
    why = "";
    return true;
}

} // namespace PyCore
