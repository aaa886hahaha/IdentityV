#ifndef IDV_PY_PROGRESS_H
#define IDV_PY_PROGRESS_H

// ============================================================================
// 密码机破译进度 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 为什么不能像坐标那样直接读结构体偏移：
//   数组里那 290 个场景对象是**渲染节点**(类名是 .gim 模型路径)，身上不带任何玩法状态。
//   对 7 台机器做过四档差分(0/25/80/100)，对象本体 0x400 字节 + 沿指针下钻两层，
//   f32/f64/u32/u16/u8 五种宽度全扫，零命中。另外 fix_process / hack_process /
//   get_generator_units 这些名字在 libclient.so 里**一个都搜不到**，说明它们不是
//   C++ 注册给 Python 的属性，而是纯 Python 脚本里定义的。进度只存在于 CPython 堆上。
//
// 完整链路(每一跳都实测验证过，跨对局重验通过)：
//   PyRoot::g_modules_slot                 -> sys.modules            (在 .so 的 .PyRuntime 段里，见 PyRoot.h)
//     按名字找 "game_kernel"          -> module
//       module + 0x10                -> md_dict                (进程内稳定，不随对局变)
//         按名字找 "unit_mgr"         -> UnitManager 实例       (每局重新分配)
//           实例 - 0x18              -> managed dict
//             按名字找 "units_by_type"-> dict(int -> list)
//               键 int 3             -> list                   (3 = 密码机的 unit_type)
//                 list+0x10=元素数 +0x18=元素数组
//                   每个 GeneratorUnit 实例 - 0x18 -> dict
//                     按名字找 "fix_process" -> 0~100 的进度
//                     按名字找 "model"       -> +0x20 = **场景对象指针**(就是数组里那个 obj)
//                     按名字找 "position"    -> +0x10/+0x14/+0x18 = X/高度/Y
//
// 配对为什么用 model 而不是坐标：实测一局里 7 台有 4 台的 position 读不出/为 0，
// 靠坐标那几台就配不上 —— 表现是"明明破译完了却还画着机器"。
// model+0x20 是指针身份，同一局 7 台里 6 台立刻命中(剩下 1 台是 model 短暂为空)，
// 命中的场景对象 +0x240 全是 2，和场景侧判据完全一致。坐标只作为 model 为空时的兜底。
//
// 解释器版本是 CPython 3.11：dict 用 PyDictUnicodeEntry{key,value} 16 字节(不存 hash)，
// PyLong 仍是 ob_size 语义(不是 3.12 的 lv_tag)，managed-dict 的 dict 指针在对象前 0x18。
//
// 三个必须守住的坑：
//   1) entries 起始 = keys + 32 + (1<<dk_log2_index_bytes)，**必须逐实例算**。
//      同一个类的不同实例 dict 大小可以不同，把一个实例算出的偏移套到别的实例上会读到无关属性。
//   2) 值的类型在 int 和 float 之间来回变(0 和 100 通常是缓存的小整数对象，中间值是 float)，
//      两种都要认；绝不能只认一种，更不能把"认不出"当成 0。
//   3) 属性每被赋值一次就换一个新的 float 对象，旧的进 freelist 被复用。
//      "读指针 -> 再解引用"之间如果隔得久，会读到已经变成别人的内存(refcount 变 0 / 类型不对)。
//      所以解引用后要**再读一次指针确认没变**，并校验 refcount>0、类型对、数值在合理区间。
//      驱动读一次是微秒级，正常情况一次就过；PC 侧用 adb 读(一次 0.3 秒)则几乎必然读到垃圾。
//
// 序号(entry index)依赖类属性的插入顺序，同版本内稳定、热更后可能位移，
// 所以这里一律**按名字查找**，查到后把序号缓存下来，下次先试缓存并用名字校验，
// 对不上就自动回退全扫。这样热更后不会给出错误结果，最多慢一帧。
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

namespace PyProgress {

// sys.modules 与 dict/int/float/True/False 由 PyRoot.h 运行时找出，这里不再写死模块偏移

static const int MAX_GENERATORS = 16;
static const int GENERATOR_UNIT_TYPE = 3;

// 大门。2026-09-22 实测 units_by_type[6] = DoorUnit，一局 2 个。
// 当时的属性序号：hack_process #220 hack_speed #221 can_open #217 has_open #218
//                is_opening #219 open_rate #226 model #63
// 这里仍然一律按名字查 + 序号缓存校验，热更后会自动重找，上面的数字只作参考。
//
// ！！大门进度只能走驱动读，不要试图用 adb/跨进程脚本验证 ！！
// hack_process 每帧都被重新赋值成一个新的 float 对象。实测 PC 侧一次 fetch 约 0.4 秒，
// 40 次取指针 40 次都已失效，命中率是 0；而驱动读"取指针->解引用->重读指针"是微秒级，
// 对 16ms 的重赋值有三四个数量级余量。下面 read_number() 的五道闸(指针重读/refcount/类型/
// 0~100 值域/重试三次)一条都不能省 —— 尤其值域那条，垃圾值里 0.16 这种"看着合理"的
// 也出现过，只靠指针稳定挡不住。
static const int MAX_DOORS = 4;
static const int DOOR_UNIT_TYPE = 6;

struct Entry { uint64_t scene_obj; float x, y, z; float progress; };
struct DoorEntry { uint64_t scene_obj; float progress; bool can_open, has_open, is_opening; };

// 红夫人镜面：units_by_type[246] = MaryMirrorUnit，一局 0/1 个。
// 2026-10-18 probe2 实测：position(镜面上一点) 与 direction(单位法向) 都是 `vector` 类型、同一布局
// (+0x10/+0x14/+0x18 = X/高度/Y)。未放下时整条在地下(高度≈-999.8)，放下即为最终值，
// 且比"本体+247 垂直平分线"早一帧可用(那一帧 247 镜中红夫人还在地下) —— 正好补上衔接空窗。
static const int MARY_MIRROR_UNIT_TYPE = 246;
struct MirrorPlane { bool valid; float px, pz, nx, nz; };   // 水平：镜面点(px,pz) + 归一化法向(nx,nz)

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到一份完整的数据
static Entry   g_buf[2][MAX_GENERATORS];
static volatile int g_count[2] = {0, 0};
// atomic 而不是 volatile：发布是"先填缓冲、再翻转下标"两步写，volatile 不阻止 ARM64 把它们
// 乱序给到别的核心 —— 绘制线程可能先看到新下标、再看到旧内容，读到正在被填写的那块。
// 读的一侧有地址依赖(下标决定读哪块)，硬件自会保序，所以只需要写侧这一道。
// 默认 seq_cst 在 ARM64 上编成 stlr/ldar，不插 dmb。
static std::atomic<int> g_active{0};

// 大门跟密码机在同一次刷新里填、共用 g_active 的那一次翻转
static DoorEntry g_door_buf[2][MAX_DOORS];
static volatile int g_door_count[2] = {0, 0};

// 镜面也在同一次刷新里填、共用 g_active 翻转
static MirrorPlane g_mirror_buf[2] = {};

// 约瑟夫镜世界的镜像密码机**场景对象**黑名单：它们不进 g_buf(没进度)，但场景对象仍在 data[] 里、
// 会被 is_real_generator 画成"[密码机]"空标记。绘制侧用 is_mirror_generator() 跳过。跟 g_active 同步翻转
static uint64_t g_mirror_gen[2][MAX_GENERATORS];
static int      g_mirror_gen_n[2] = {0, 0};

static uintptr_t g_libbase = 0;
static uint64_t  g_module_dict = 0;                 // game_kernel 的 md_dict，进程内稳定
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1, g_i_fix = -1, g_i_pos = -1, g_i_model = -1;
// 大门自己的一套序号缓存：DoorUnit 和 GeneratorUnit 是两个类，插入顺序不同，
// 序号不能共用(共用的话每个单位都要重扫一遍，白白浪费)
static int64_t   g_i_hack = -1, g_i_door_model = -1, g_i_can_open = -1, g_i_has_open = -1, g_i_is_opening = -1;
static int64_t   g_i_m_pos = -1, g_i_m_dir = -1;    // MaryMirrorUnit 的 position/direction 序号缓存
static volatile bool g_available = false;
static volatile int  g_fail_streak = 0;
static char g_status[96] = "未启动";

// ---- 公共 CPython 工具：实现在 PyCore.h，三个模块共用一份(见那里的文件头) ----
using PyCore::is_obj; using PyCore::str_equals; using PyCore::Dict; using PyCore::read_dict;
using PyCore::key_slot; using PyCore::value_slot; using PyCore::find_key; using PyCore::get_attr;
using PyCore::resolve_index; using PyCore::get_type_list; using PyCore::read_bool; using PyCore::read_list;

// 进度是 0~100 的百分比。**这道值域闸是挡住垃圾值的主力**，垃圾里出现过 0.16 这种看着合理的，
// 只靠指针稳定挡不住。冷却是 0~600 秒级，两者不能混用，所以值域留在各模块自己手里
static inline bool read_number(uint64_t slot, float &out) { return PyCore::read_number(slot, out, -0.5, 100.5); }

// 约瑟夫镜世界("photo world")：镜像密码机/大门的 out_photo_uid 指向真身(真身的是 None)。
// 用户要求：只隐藏镜像那份，保留真实世界的进度/门。2026-10-22 约瑟夫局 probe2 实测(cmd_joseph.py)：
// units_by_type[3] 7 真(in_photo_uid 有值) + 7 镜像(out_photo_uid 有值)。非约瑟夫局这属性恒 None,恒 false。
static int64_t g_i_outphoto = -1;   // out_photo_uid 的共用序号缓存(密码机/大门两类都用；resolve 会自愈)
static inline bool is_photo_ghost(uint64_t d, const Dict &dk)
{
    g_i_outphoto = resolve_index(d, dk, g_i_outphoto, "out_photo_uid");
    if (g_i_outphoto < 0) return false;
    int64_t v = 0;
    return PyCore::read_int(getPtr64(value_slot(dk, g_i_outphoto)), v) && v > 0;
}

static bool resolve_module()
{
    const char *why = "";
    g_module_dict = PyCore::game_kernel_dict(why);
    if (!g_module_dict) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }
    return true;
}

// 大门：units_by_type[6]。读不到不算整轮失败 —— 门和密码机互不影响
static void collect_doors(uint64_t ubt, int write_idx)
{
    g_door_count[write_idx] = 0;
    uint64_t lst = get_type_list(ubt, DOOR_UNIT_TYPE);
    if (!is_obj(lst)) return;

    int64_t n = 0;
    vm_readv(lst + 0x10, &n, 8);                 // ob_size
    uint64_t items = getPtr64(lst + 0x18);       // ob_item
    if (n <= 0 || n > 16 || !is_obj(items)) return;
    if (n > MAX_DOORS) n = MAX_DOORS;

    int count = 0;
    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!is_obj(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);
        Dict dk;
        if (!read_dict(d, dk)) continue;
        if (is_photo_ghost(d, dk)) continue;   // 约瑟夫镜世界的镜像大门：跳过(只画真实世界的门)

        g_i_hack    = resolve_index(d, dk, g_i_hack,    "hack_process");
        g_i_door_model = resolve_index(d, dk, g_i_door_model, "model");
        g_i_can_open    = resolve_index(d, dk, g_i_can_open,    "can_open");
        g_i_has_open    = resolve_index(d, dk, g_i_has_open,    "has_open");
        g_i_is_opening  = resolve_index(d, dk, g_i_is_opening,  "is_opening");
        if (g_i_hack < 0) continue;

        float progress = 0.f;
        if (!read_number(value_slot(dk, g_i_hack), progress)) continue;   // 读不稳就整扇门跳过，不发布垃圾

        // 配对只能用 model 的指针身份。大门**不能**走场景侧判据：
        // 实测两扇门的场景类名还不一样(dm65_scene_prop_30 / dm65_scene_wooddoor01a)，
        // 而且两扇的 +0x240 都是 0 —— 那条"最可靠的存在性判据"在大门上直接失效。
        uint64_t scene = 0;
        if (g_i_door_model >= 0) {
            uint64_t mo = getPtr64(value_slot(dk, g_i_door_model));
            if (is_obj(mo)) {
                uint64_t sp = getPtr64(mo + 0x20);
                if (is_obj(sp)) scene = sp;
            }
        }
        if (scene == 0) continue;                  // 没有身份就画不到屏幕上，收了也没用

        bool can_open = false, has_open = false, opening = false;
        if (g_i_can_open   >= 0) read_bool(value_slot(dk, g_i_can_open),   can_open);
        if (g_i_has_open   >= 0) read_bool(value_slot(dk, g_i_has_open),   has_open);
        if (g_i_is_opening >= 0) read_bool(value_slot(dk, g_i_is_opening), opening);

        g_door_buf[write_idx][count].scene_obj = scene;
        g_door_buf[write_idx][count].progress     = progress;
        g_door_buf[write_idx][count].can_open     = can_open;
        g_door_buf[write_idx][count].has_open     = has_open;
        g_door_buf[write_idx][count].is_opening   = opening;
        count++;
    }
    g_door_count[write_idx] = count;
}

// 红夫人镜面：units_by_type[246][0] 的 position(点) + direction(法向)。
// 读不到不算整轮失败，和大门一样独立。只在红夫人局会有这个单位。
static void collect_mirror(uint64_t ubt, int write_idx)
{
    g_mirror_buf[write_idx].valid = false;
    uint64_t lst = get_type_list(ubt, MARY_MIRROR_UNIT_TYPE);
    if (!is_obj(lst)) return;                        // 没有镜子单位(没放过/不是红夫人局)
    int64_t n = 0;
    vm_readv(lst + 0x10, &n, 8);
    uint64_t items = getPtr64(lst + 0x18);
    if (n <= 0 || !is_obj(items)) return;
    uint64_t inst = getPtr64(items);                 // 只有 1 个 MaryMirrorUnit，取第 0 个
    if (!is_obj(inst)) return;
    uint64_t d = getPtr64(inst - 0x18);
    Dict dk;
    if (!read_dict(d, dk)) return;

    g_i_m_pos = resolve_index(d, dk, g_i_m_pos, "position");
    g_i_m_dir = resolve_index(d, dk, g_i_m_dir, "direction");
    if (g_i_m_pos < 0 || g_i_m_dir < 0) return;

    // position / direction 都是 vector 类型，+0x10/+0x14/+0x18 = X/高度/Y(实测 same_type)。
    // 取指针->读 12 字节->重读指针确认没变(防竞态，同密码机那套)。
    float pos[3], dir[3];
    uint64_t pp = getPtr64(value_slot(dk, g_i_m_pos));
    if (!is_obj(pp) || !vm_readv(pp + 0x10, pos, 12)) return;
    if (getPtr64(value_slot(dk, g_i_m_pos)) != pp) return;
    uint64_t dp = getPtr64(value_slot(dk, g_i_m_dir));
    if (!is_obj(dp) || !vm_readv(dp + 0x10, dir, 12)) return;
    if (getPtr64(value_slot(dk, g_i_m_dir)) != dp) return;

    if (!(std::isfinite(pos[0]) && std::isfinite(pos[2]) && std::isfinite(dir[0]) && std::isfinite(dir[2]))) return;
    if (pos[1] <= -300.f) return;                    // pos[1]=高度；未放下时在地下(≈-999.8)
    float nlen = sqrtf(dir[0] * dir[0] + dir[2] * dir[2]);   // 水平法向模(实测整向量 |dir|=1，竖直分量≈0)
    if (nlen < 0.1f) return;                          // 法向几乎竖直/无效
    g_mirror_buf[write_idx].px = pos[0];
    g_mirror_buf[write_idx].pz = pos[2];
    g_mirror_buf[write_idx].nx = dir[0] / nlen;       // 归一化水平法向
    g_mirror_buf[write_idx].nz = dir[2] / nlen;
    g_mirror_buf[write_idx].valid = true;
}

static bool try_refresh()
{
    if (g_module_dict == 0 && !resolve_module()) return false;

    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? get_attr(g_module_dict, g_i_unit_mgr) : 0;
    if (!is_obj(um)) { snprintf(g_status, sizeof(g_status), "unit_mgr 无效(未在对局中?)"); return false; }

    uint64_t ud = getPtr64(um - 0x18);            // managed-dict 预头
    g_i_ubt = find_key(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = (g_i_ubt >= 0) ? get_attr(ud, g_i_ubt) : 0;
    if (!is_obj(ubt)) { snprintf(g_status, sizeof(g_status), "units_by_type 无效"); return false; }

    // 大门、镜面先收：放在密码机那几个 return false 之前，免得"没有密码机列表"把它们也带下水
    int write_idx = 1 - g_active;
    g_mirror_gen_n[write_idx] = 0;                 // 约瑟夫镜世界镜像密码机黑名单，本轮重建
    collect_doors(ubt, write_idx);
    collect_mirror(ubt, write_idx);

    // 在 int->list 的表里找键 3
    Dict k;
    if (!read_dict(ubt, k)) { snprintf(g_status, sizeof(g_status), "units_by_type 结构异常"); return false; }
    uint64_t lst = 0;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (!is_obj(kp) || getPtr64(kp + 8) != PyRoot::g_int_type) continue;
        int64_t sz = 0; uint32_t dg = 0;
        vm_readv(kp + 16, &sz, 8);
        vm_readv(kp + 24, &dg, 4);
        if (sz == 1 && (int)dg == GENERATOR_UNIT_TYPE) { lst = getPtr64(value_slot(k, i)); break; }
    }
    if (!is_obj(lst)) { snprintf(g_status, sizeof(g_status), "没有密码机列表"); return false; }

    int64_t n = 0;
    vm_readv(lst + 0x10, &n, 8);                 // ob_size
    uint64_t items = getPtr64(lst + 0x18);       // ob_item
    if (n <= 0 || n > 64 || !is_obj(items)) { snprintf(g_status, sizeof(g_status), "密码机列表异常"); return false; }
    if (n > MAX_GENERATORS) n = MAX_GENERATORS;

    int count = 0;                                  // 写缓冲下标在上面收大门时已经取好
    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!is_obj(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);
        Dict dk;
        if (!read_dict(d, dk)) continue;
        if (is_photo_ghost(d, dk)) {   // 约瑟夫镜世界的镜像密码机：不进进度缓冲，场景对象记进黑名单供绘制侧跳过
            g_i_model = resolve_index(d, dk, g_i_model, "model");
            if (g_i_model >= 0) {
                uint64_t mo = getPtr64(value_slot(dk, g_i_model));
                if (is_obj(mo)) {
                    uint64_t sp = getPtr64(mo + 0x20);
                    if (is_obj(sp) && g_mirror_gen_n[write_idx] < MAX_GENERATORS)
                        g_mirror_gen[write_idx][g_mirror_gen_n[write_idx]++] = sp;
                }
            }
            continue;
        }

        g_i_fix   = resolve_index(d, dk, g_i_fix,   "fix_process");
        g_i_model = resolve_index(d, dk, g_i_model, "model");
        g_i_pos   = resolve_index(d, dk, g_i_pos,   "position");
        if (g_i_fix < 0) continue;

        float progress = 0.f;
        if (!read_number(value_slot(dk, g_i_fix), progress)) continue;

        uint64_t scene = 0;
        if (g_i_model >= 0) {
            uint64_t mo = getPtr64(value_slot(dk, g_i_model));
            if (is_obj(mo)) {
                uint64_t sp = getPtr64(mo + 0x20);
                if (is_obj(sp)) scene = sp;
            }
        }
        float xyz[3] = {0, 0, 0};
        if (g_i_pos >= 0) {
            uint64_t pos = getPtr64(value_slot(dk, g_i_pos));
            if (is_obj(pos)) vm_readv(pos + 0x10, xyz, 12);   // +0x10/+0x14/+0x18 = X/高度/Y
        }
        g_buf[write_idx][count].scene_obj = scene;
        g_buf[write_idx][count].x = xyz[0];
        g_buf[write_idx][count].z = xyz[1];
        g_buf[write_idx][count].y = xyz[2];
        g_buf[write_idx][count].progress = progress;
        count++;
    }

    if (count == 0) { snprintf(g_status, sizeof(g_status), "一台也没读出"); return false; }
    g_count[write_idx] = count;
    g_active = write_idx;                                   // 填完再翻转，绘制线程永远看到完整的一份
    snprintf(g_status, sizeof(g_status), "正常 %d 台 门%d", count, g_door_count[write_idx]);
    return true;
}

static void refresh_once()
{
    if (g_libbase == 0) return;
    if (!PyRoot::ensure()) {
        g_available = false;
        snprintf(g_status, sizeof(g_status), "%s", PyRoot::status_text());
        return;
    }
    if (try_refresh()) {
        g_available = true;
        g_fail_streak = 0;
        return;
    }
    g_available = false;
    if (++g_fail_streak >= 8) {                       // 连续失败就把缓存的序号全部作废，下一轮重新按名字找
        g_module_dict = 0;
        g_i_unit_mgr = g_i_ubt = g_i_fix = g_i_pos = g_i_model = -1;
        g_i_hack = g_i_door_model = g_i_can_open = g_i_has_open = g_i_is_opening = -1;
        g_i_outphoto = -1;
        g_fail_streak = 0;
    }
}

// 一轮大约几十到两百次驱动读(微秒级)，100ms 一轮的开销可以忽略，换来 10Hz 的刷新
static const int REFRESH_INTERVAL_MS = 100;

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
    g_libbase   = libbase;
    snprintf(g_status, sizeof(g_status), "启动中");
    std::thread(thread_main).detach();
    printf("[密码机进度] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool available() { return g_available; }
static inline const char *status_text() { return g_status; }

// 把场景对象配到对应的 GeneratorUnit。
// 首选指针身份(model+0x20 就是场景对象本身)；model 为空时才退回坐标近邻。
// 坐标兜底不能单独用: 实测一局 7 台里有 4 台的 position 读不出，那几台会一直配不上。
static bool lookup(uintptr_t obj, float x, float y, float &progress)
{
    if (!g_available) return false;
    int b = g_active, n = g_count[b];
    for (int i = 0; i < n; i++) {
        if (g_buf[b][i].scene_obj == (uint64_t)obj) { progress = g_buf[b][i].progress; return true; }
    }
    int nearest = -1; float min_dist = 2.0f;
    for (int i = 0; i < n; i++) {
        if (g_buf[b][i].scene_obj != 0) continue;                        // 有身份的已经在上面比过了
        if (g_buf[b][i].x == 0.f && g_buf[b][i].y == 0.f) continue;    // 坐标没写入的跳过，避免误配
        float d = fabsf(g_buf[b][i].x - x) + fabsf(g_buf[b][i].y - y);
        if (d < min_dist) { min_dist = d; nearest = i; }
    }
    if (nearest < 0) return false;
    progress = g_buf[b][nearest].progress;
    return true;
}

static inline bool is_decoded(float progress) { return progress >= 99.95f; }

// 枚举本局全部密码机(含 0%、含已破译)，给左侧汇总面板用。
// 绘制侧自行按 is_decoded() 过滤掉修完的，使其从面板消失。
static inline int gen_total() { return g_available ? g_count[g_active] : 0; }

// 约瑟夫镜世界：这个场景对象是不是镜像密码机(要在绘制侧跳过，不画"[密码机]"空标记)
static bool is_mirror_generator(uintptr_t obj)
{
    if (!g_available) return false;
    int b = g_active, n = g_mirror_gen_n[b];
    for (int i = 0; i < n; i++) if (g_mirror_gen[b][i] == (uint64_t)obj) return true;
    return false;
}
static bool get_gen(int i, float &progress)
{
    if (!g_available) return false;
    int b = g_active;
    if (i < 0 || i >= g_count[b]) return false;
    progress = g_buf[b][i].progress;
    return true;
}

// ---------------- 大门 ----------------
// 大门不进 data[] 数组(getscene() 只认 prop_76/sender，大门那条分支收不到)，
// 所以绘制侧不走实体循环，直接遍历这里拿到场景对象再自己投影。
static inline int door_count() { return g_available ? g_door_count[g_active] : 0; }

static bool get_door(int i, DoorEntry &out)
{
    if (!g_available) return false;
    int b = g_active;
    if (i < 0 || i >= g_door_count[b]) return false;
    out = g_door_buf[b][i];
    return true;
}

// 红夫人镜面(权威来源)：已放下的镜子的 镜面点(px,pz) + 归一化水平法向(nx,nz)。
// 返回 false = 没放下/不是红夫人局/读不到 —— 此时绘制侧退回老的"本体+247 垂直平分线"/预览模型。
static bool mirror_plane(float &px, float &pz, float &nx, float &nz)
{
    if (!g_available) return false;
    int b = g_active;
    if (!g_mirror_buf[b].valid) return false;
    px = g_mirror_buf[b].px; pz = g_mirror_buf[b].pz;
    nx = g_mirror_buf[b].nx; nz = g_mirror_buf[b].nz;
    return true;
}

// 大门开启完成。hack_process 是 0~100 的百分比(跟 fix_process 同构，
// 作者载荷里也是按 "进度:{:.1f}%" 格式化的)
static inline bool door_opened(const DoorEntry &d) { return d.has_open || d.progress >= 99.95f; }

static int decoded_count()
{
    if (!g_available) return -1;
    int b = g_active, n = g_count[b], c = 0;
    for (int i = 0; i < n; i++) if (is_decoded(g_buf[b][i].progress)) c++;
    return c;
}

// 一局要破译 5 台。已破译 完成 台 -> 还需 需要 = 5 - 完成 台 -> 在还没破译、且已经有进度的
// 机器里按进度降序取第 需要 名，那台就是最后修完的（前面 需要-1 台都会比它先完成）。
// 有进度的机器不足 需要 台时返回 false：剩下的名额会落在某台 0% 的机器上，无从判断是哪台。
// 只在 完成 == 4（即 需要 == 1）时才给结果：此时进度最高的那台就是最后一台，是确定的。
// 完成 < 4 时的第 需要 名只是按当前进度的预测，中途换人修就会变，所以不显示。
static bool last_generator(float &progress)
{
    if (!g_available) return false;
    int b = g_active, n = g_count[b];
    int done = 0;
    float in_progress[MAX_GENERATORS];
    int m = 0;
    for (int i = 0; i < n; i++) {
        float v = g_buf[b][i].progress;
        if (is_decoded(v)) { done++; continue; }
        if (v > 0.05f) in_progress[m++] = v;
    }
    if (done < 4) return false;                    // 不满 4 台时不显示：名次还会变
    int need = 5 - done;
    if (need < 1 || m < need) return false;         // 已破译满 5 台则没有"最后一台"可言
    for (int i = 0; i < m - 1; i++)                 // 只有几个元素，插入排序足够
        for (int j = i + 1; j < m; j++)
            if (in_progress[j] > in_progress[i]) { float t = in_progress[i]; in_progress[i] = in_progress[j]; in_progress[j] = t; }
    progress = in_progress[need - 1];                       // 降序第 需要 名
    return true;
}

} // namespace 密码机进度

#endif // IDV_PY_PROGRESS_H
