#ifndef IDV_PY_GENIUS_H
#define IDV_PY_GENIUS_H

// ============================================================================
// 天赋与辅助特质 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 链路跟 PyProgress.h 完全一样(sys.modules -> game_kernel -> unit_mgr ->
// units_by_type)，只是取的是 1(ButcherUnit)、2(CivilianUnit)、236(梦之信徒，只读冷却) 三个键，
// 读的属性不同。CPython 3.11 的结构布局、防竞态读法、按名字查找+序号缓存
// 这些共通的东西在 PyProgress.h 文件头有完整说明，这里不重复。
//
// 属性：
//   genius_id_lv_lst   list[[天赋id, 等级], ...]
//   support_skill_id   list[id]，监管的辅助特质；求生者也有这个字段
//   model              -> +0x20 = 场景对象指针，用来跟绘制循环里的 obj 配对
//   position           -> +0x10/+0x14/+0x18 = X/高度/Y，model 为空时的兜底
//
// ！！两个必须记住的坑 ！！
//
// 1) **活体属性名和录像里的不一样**：录像快照里是 `genius_id_lvs`，
//    Python 层是 `genius_id_lv_lst`。照录像的名字去查会一个也找不到。
//
// 2) **监管和求生者共用同一套天赋 id 号段，同一个 id 在两边是不同天赋。**
//    尤其 id 26 在求生者侧是绝处逢生，在监管侧是另一个(还没查清的)天赋 ——
//    所以"有绝处逢生就标绿"这条只能对 阵营==2 生效，绝不能按 id 直接判。
//
// 3) **genius_id_lv_lst 经常读不出内容**(PC 侧脚本里表现成空列表或读不到)，
//    而同一时刻隔壁的 support_skill_id 每次都好。**原因还没定论。**
//    最合理的假设是冷热页差异：support_skill_id 是界面一直在用的热数据、结构也浅；
//    genius_id_lv_lst 是十几个嵌套 list，要碰的页多得多，而且开局读一次之后
//    再没人碰，是冷页 —— 内存压力下优先被换进 zram。
//
//    ⚠ 注意这里的读法：本文件的 read_genius() 自己读 ob_size，能分清"真的空"和"读不到"。
//    PC 侧那些脚本用的 pyw.list_items() 把这两种情况都返回 []，**不能拿它判断列表是不是空的**。
//
//    **做法：按 uid 增量累积 + 二次确认**(见 read_genius())。天赋一局之内不会变，所以每轮能读到几条
//    就合并几条进记忆，哪怕每一轮都只读到一部分，记忆也会逐渐补全：
//      - **一轮就读全**(n 条全部合法且 id 不重复)直接判完整 —— 天赋页是冷页，内存压力下
//        被压进 zram 后驱动读不回来，必须趁热一次拿下(2026-09-22 VmSwap 1.1G 时天赋全不显示)
//      - 没读全时，单条 [id, 等级] 要在**两个不同轮次读到相同值**才确认，挡住偶发垃圾(进记忆就是一整局)
//      - 列表长度 ob_size 本身读得稳，记为 n；已确认条数 == n 才算"完整"
//      - 只有"完整"时才能断言"没带某个天赋"；不完整时只能说"已确认带了哪些"
//    旧做法是"单次读到 >=2 个大天赋(求生者还要有 26)才锁存"，把"读全了没有"和"一般人怎么带"
//    混在一起判，导致少带大天赋/没带绝处逢生的人永远锁不上，而且"没带"和"读失败"分不开。
//
//    记忆的清理时机：**unit_mgr 地址变了才清**。uid 每局都从 1000001 重新开始，
//    不清的话下一局会套用上一局同号玩家的天赋，那是实打实的错误信息。
//    而 unit_mgr 实例每局重新分配(见 python-layer.md 的生命周期)，
//    地址一变就说明换局了，这个信号比"连续失败 N 轮"准得多 ——
//    后者在对局中途链路短暂中断(内存压力把 CPython 堆压进 zram)时会误清。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <thread>
#include <ctime>
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"
#include "PyCore.h"

namespace PyGenius {

// sys.modules 与 dict/int/float/True/False 由 PyRoot.h 运行时找出，这里不再写死模块偏移

static const int MAX_UNITS = 24;   // 1 监管 + 4 求生者 + 梦之信徒(实测一局 5 个) + 余量
static const int BUTCHER_UNIT_TYPE  = 1;
static const int CIVILIAN_UNIT_TYPE = 2;
// 梦之女巫的信徒(YidhraPuppetUnit)。2026-09-22 实测(求生者视角读对面女巫)：每个信徒有**自己的**
// skill_mgr / skill_dict / 711 闪现技能对象，_cd_delta 各自独立递减；support_skill_id 与本体相同([7])。
// 所以按特质找主技能那套逻辑原样适用。信徒不读天赋/绝处逢生，只要冷却
static const int YIDHRA_PUPPET_UNIT_TYPE = 236;

// 绝处逢生的**被动技能 id**(不是天赋 id)。天赋 id 是 26，落地成被动是 102。
// unit.ability_used 是 dict{被动id: bool}，True = 本局已消耗。
// 2026-09-22 实测：非本机的 CivilianUnit 上读到 {102: True}，
// 说明服务器**会把别人的消耗状态下发给本机**，不是只有自己的能看。
// 这个属性只存在于求生者类上，监管(MyButcherUnit)身上根本没有。
static const int PASSIVE_DESPERATE = 102;

// 飞轮效应(求生者天赋 8，被动 207)的**技能 id**。被动配表
// passive_skill_data[(207,1)]['active_skills'] = (11111,)，冷却挂在 skill_dict[11111] 上：
// SkillFlywheelSprint，cd_time 135、game_start_cd 50，_cd_delta 每秒 -1(2026-09-22 注入实测)。
// ⚠ 配表里那个 skill_cd = 10.0 不是飞轮冷却。
// ⚠ 只在本机玩家身上验过；非本机求生者的 skill_dict 里有没有它还没验(那局没人带)。
static const int SKILL_FLYWHEEL = 11111;

// 天赋 id -> 位号。只关心四个分支终端 + 26(求生者的绝处逢生)，其余忽略。
// 两个阵营共用这张 id->位 的映射，名字在格式化时才按阵营区分。
enum {
    BIT_ID_8  = 1 << 0,   // id 8 : 求生=飞轮效应  监管=紧闭空间
    BIT_ID_16 = 1 << 1,  // id 16: 求生=回光返照  监管=底牌
    BIT_ID_24 = 1 << 2,  // id 24: 求生=化险为夷  监管=挽留
    BIT_ID_26 = 1 << 3,  // id 26: 求生=绝处逢生  监管=未知
    BIT_ID_32 = 1 << 4,  // id 32: 求生=膝跳反射  监管=张狂
};

struct Info {
    int      camp;       // 1=监管 2=求生 236=梦之信徒(只有特质冷却，天赋状态恒为未知)
    uint32_t genius_bits;     // 只含**已确认**的条目
    int      support_trait;   // 1..8，0=没读到
    int      genius_state;   // GENIUS_UNKNOWN / GENIUS_PARTIAL / GENIUS_COMPLETE，见 read_genius()
    uint8_t  genius_levels[41]; // 已确认的 {天赋id: 等级}，0 = 未确认或没带。下标 1~40
    bool     desperate_used;   // ability_used[102] == True，本局已经触发过绝处逢生
    // 监管辅助特质的冷却(求生者侧全为 0)
    bool     has_cd;     // false = 没读到技能对象
    float    cd_remain;   // 秒，0 = 就绪。**读到那一刻的值**，显示时用 cd_remain_now() 往下推
    int64_t  cd_read_ms;   // now_ms() 时钟，配合 冷却剩余 做推算
    float    cd_rate;   // skill_mgr.cd_rate，实测恒 1；推算时乘上它
    float    cd_total;   // cd_time，画进度环用
    int      charge_max;   // power_num，非充能型为 0
    int      charge_cur;   // _cur_power_num
    // 求生者飞轮效应的冷却(skill_dict[SKILL_FLYWHEEL])，监管侧全为 0
    bool     has_flywheel;     // skill_dict 里有飞轮技能且读到了 _cd_delta
    float    flywheel_remain;   // 秒，0 = 就绪。读到那一刻的值
    int64_t  flywheel_read_ms;   // now_ms() 时钟
    float    flywheel_rate;   // skill_mgr.cd_rate
};

struct Entry {
    uint64_t scene_obj;
    float    x, y;
    Info     info;
};

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到完整的一份
static Entry g_buf[2][MAX_UNITS];
static volatile int g_count[2] = {0, 0};
// 花名册(见 read_roster())跟着同一次翻转发布。局外没有单位，g_count 会是 0，但花名册仍有内容
static const int MAX_ROSTER = 12;   // 联合狩猎 2 + 8 = 10 人(2026-10-25 从 8 扩)
struct RosterEntry {
    char     uid[24];      // 真人的账号 id 字符串；人机为空串
    int64_t  eid;          // 人机的 eid(int)；真人是 ObjectId，读不成 int 时为 -1
    int      role_type;    // 1=监管 2=求生
    int      pid;          // 角色 id
    uint32_t bits;         // 终端天赋位(监管恒为 0，游戏不公开监管天赋)
    int      seat;         // 准备界面座位号 1..4(UIRoomWait.uid2pos)，0 = 不知道。只有准备阶段求生者条目填
};
static RosterEntry g_roster_buf[2][MAX_ROSTER];
static volatile int g_roster_cnt[2] = {0, 0};
static RosterEntry g_civ_buf[2][MAX_ROSTER];
static volatile int g_civ_cnt[2] = {0, 0};
static std::atomic<int> g_active{0};      // 见 PyProgress.h 同名变量的注释(volatile 不保证发布顺序)
// 本机在准备房间里是监管(UIRoomWait.role == 1)。每轮重算，没有面板/读不到一律 false。
// 准备阶段的求生者天赋只在这个为真时显示(用户要求：自己是求生者时不需要)
static std::atomic<bool> g_prep_as_hunter{false};

// 天赋锁存：genius_id_lv_lst 会间歇性读空(见文件头第 3 条)，按 uid 记住最后一次非空的结果
enum { GENIUS_UNKNOWN = 0, GENIUS_PARTIAL = 1, GENIUS_COMPLETE = 2 };

// 每个玩家的天赋记忆(按 uid)，增量累积，见 read_genius()
struct GeniusMemory {
    int64_t uid;
    int     n;              // genius_id_lv_lst 的长度(ob_size)，0 = 还没读到过
    int     confirmed;
    uint8_t levels[41];       // 已确认的等级，0 = 未确认
    uint8_t candidates[41];       // 读到过一次、等待第二次确认的等级
};
static GeniusMemory g_mem[MAX_UNITS];
static int      g_mem_count = 0;
static uint64_t g_last_unit_mgr = 0;    // 地址一变就说明换局了，此时才清记忆

static uintptr_t g_libbase = 0;
static uint64_t  g_module_dict = 0;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_genius = -1, g_i_support = -1, g_i_model = -1, g_i_pos = -1, g_i_uid = -1;
static int64_t   g_i_unique = -1;   // unit.unique_id，用来和花名册配对
static int64_t   g_i_ability = -1;   // ability_used，只有求生者类上有
// 监管技能冷却链：unit.skill_mgr -> skill_dict{id: Skill} / leader_skills(set)
static int64_t   g_i_skillmgr = -1, g_i_skilldict = -1, g_i_cdrate = -1;
// Skill 对象上的属性(223 条)，各自一套缓存
static int64_t   g_i_cd = -1, g_i_cdtime = -1, g_i_power = -1, g_i_curpower = -1;

static void clear_mem(GeniusMemory &m, int64_t uid)
{
    memset(&m, 0, sizeof(m));
    m.uid = uid;
}

// 取某个 uid 的记忆，没有就新建；表满了返回 nullptr
static GeniusMemory *get_mem(int64_t uid)
{
    for (int i = 0; i < g_mem_count; i++)
        if (g_mem[i].uid == uid) return &g_mem[i];
    if (g_mem_count >= MAX_UNITS) return nullptr;
    clear_mem(g_mem[g_mem_count], uid);
    return &g_mem[g_mem_count++];
}
static volatile bool g_available = false;
static volatile int  g_fail_streak = 0;
static char g_status[96] = "";   // 空 = 未启动，见 status_text()

// ---- 公共 CPython 工具：实现在 PyCore.h，三个模块共用一份(见那里的文件头) ----
using PyCore::is_obj; using PyCore::str_equals; using PyCore::Dict; using PyCore::read_dict;
using PyCore::key_slot; using PyCore::value_slot; using PyCore::find_key; using PyCore::get_attr;
using PyCore::resolve_index; using PyCore::get_type_list; using PyCore::read_int; using PyCore::read_list;
using PyCore::get_inst_dict; using PyCore::read_str;

// 冷却是 0~600 秒级(闪现 csv_cd_time 就 150)，**不能套进度那条 0~100 的闸**
static inline bool read_float(uint64_t slot, float &out, float lo, float hi) { return PyCore::read_number(slot, out, lo, hi); }

static bool resolve_module()
{
    const char *why = "";
    g_module_dict = PyCore::game_kernel_dict(why);
    if (!g_module_dict) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }
    return true;
}

// 读线程和绘制线程共用的单调时钟，冷却推算用
static inline int64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 天赋 id 的合法等级。来自配表(genius-table.md 的 lv 列)，求生者和监管这组 id 完全相同：
// 层 2.2 和 3.1/3.2/3.3 的节点可加 1~3 级，其余(分支根、2.1/2.3、终端、33~40)只有 1 级。
// 用来挡垃圾：读到的 [id, 等级] 不在配表允许范围里就不认。
static bool level_valid(int64_t id, int64_t lv)
{
    if (id < 1 || id > 40) return false;
    if (lv == 1) return true;
    if (lv < 1 || lv > 3) return false;
    if (id > 32) return false;                        // 33~40 只有 1 级
    int r = (int)(id % 8);
    return r == 3 || r == 5 || r == 6 || r == 7;      // 3/5/6/7、11/13/14/15、… 可加到 3 级
}

// genius_id_lv_lst 是 list[list[id, lv]]。每轮读一次，**增量合并**进这个玩家的记忆：
//   - 某一轮一次读全(合法不重复条数 == n)就直接判完整，见函数里的说明。
//   - 没读全时：每条 [id, 等级] 第一次读到进"候选"，之后某一轮读到相同值才"确认"(挡偶发垃圾)。
//     候选和再读到的值不一致，就用新值重新当候选，不确认。
//   - 列表长度 n 记在记忆里；已确认条数 == n 就是完整。
//   - n 变了(同一局同一个人不该变)说明记忆不可信，整份重来。
//   - 读到空列表(n==0)不动记忆：天赋一局不会变，空只可能是还没同步或已被清理。
// 返回 false = 这一轮连列表头都没读到；记忆不受影响。
static bool read_genius(uint64_t slot, GeniusMemory &mem)
{
    uint64_t lp = getPtr64(slot);
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lp, n, items)) return false;
    if (getPtr64(slot) != lp) return false;             // 解引用期间被重新赋值了
    if (n == 0) return true;
    if (n > 40) return false;                         // 天赋一共就 40 个，再多一定是垃圾

    if (mem.n != 0 && mem.n != (int)n) clear_mem(mem, mem.uid);
    mem.n = (int)n;

    // 先把这一轮读到的合法条目收齐，再决定怎么合并
    uint8_t this_round[41] = {0};
    int     this_round_n = 0;
    for (int64_t i = 0; i < n; i++) {
        uint64_t pair = getPtr64(items + i * 8);
        int64_t m = 0; uint64_t pit = 0;
        if (!read_list(pair, m, pit) || m != 2) continue;  // 单个条目读不到就跳过，下一轮再补
        int64_t id = 0, lv = 0;
        if (!read_int(getPtr64(pit), id)) continue;
        if (!read_int(getPtr64(pit + 8), lv)) continue;
        if (!level_valid(id, lv)) continue;
        if (this_round[id]) continue;                         // 同一轮里重复出现，不算两次
        this_round[id] = (uint8_t)lv;
        this_round_n++;
    }

    // 一轮就读全(合法且不重复的条数 == n)：直接判完整，不等第二轮。
    // 天赋页是冷页，开局读一次后游戏不再碰，内存压力下会被压进 zram 且再也换不回来；
    // 页还热的时候一次拿下，比等两轮确认可靠得多。垃圾值要同时满足
    // "n 条全部是合法 [id, 等级]、id 互不重复"几乎不可能。
    // 跟已确认的记忆冲突，说明有一方是垃圾：整份重来，这一轮的值只当候选。
    if (this_round_n == (int)n) {
        bool conflict = false;
        for (int id = 1; id <= 40; id++)
            if (mem.levels[id] != 0 && mem.levels[id] != this_round[id]) { conflict = true; break; }
        if (conflict) {
            clear_mem(mem, mem.uid);
            mem.n = (int)n;
            memcpy(mem.candidates, this_round, sizeof(mem.candidates));
        } else {
            memcpy(mem.levels, this_round, sizeof(mem.levels));
            mem.confirmed = (int)n;
        }
        return true;
    }

    // 没读全：逐条增量合并，两个不同轮次读到相同值才确认
    for (int id = 1; id <= 40; id++) {
        int lv = this_round[id];
        if (lv == 0) continue;
        if (mem.levels[id] != 0) continue;                 // 已确认，天赋一局不变
        if (mem.candidates[id] == (uint8_t)lv) {              // 两个不同轮次读到相同值 -> 确认
            mem.levels[id] = (uint8_t)lv;
            mem.confirmed++;
        } else {
            mem.candidates[id] = (uint8_t)lv;
        }
    }
    // 确认的比列表还多，说明有垃圾混进来了，整份重来
    if (mem.confirmed > mem.n) clear_mem(mem, mem.uid);
    return true;
}

// 记忆 -> 显示用的位掩码 / 状态
static uint32_t mem_bits(const GeniusMemory &mem)
{
    uint32_t bits = 0;
    if (mem.levels[8])  bits |= BIT_ID_8;
    if (mem.levels[16]) bits |= BIT_ID_16;
    if (mem.levels[24]) bits |= BIT_ID_24;
    if (mem.levels[26]) bits |= BIT_ID_26;
    if (mem.levels[32]) bits |= BIT_ID_32;
    return bits;
}

static int mem_state(const GeniusMemory &mem)
{
    if (mem.confirmed == 0) return GENIUS_UNKNOWN;
    return (mem.n > 0 && mem.confirmed == mem.n) ? GENIUS_COMPLETE : GENIUS_PARTIAL;
}

// ability_used = dict{被动技能id: bool}。找键 102(绝处逢生)，值为 True 就是本局已消耗。
// 键不在表里 = 还没用过(实测没用过的人这个 dict 是空的 {})，所以"找不到"要返回 false 而不是失败。
static bool read_desperate_used(uint64_t slot)
{
    uint64_t dp = getPtr64(slot);
    if (!is_obj(dp) || getPtr64(dp + 8) != PyRoot::g_dict_type) return false;
    Dict k;
    if (!read_dict(dp, k)) return false;
    for (int64_t i = 0; i < k.n_entries; i++) {
        int64_t id = 0;
        if (!read_int(getPtr64(key_slot(k, i)), id) || id != PASSIVE_DESPERATE) continue;
        uint64_t vp = getPtr64(value_slot(k, i));
        if (vp == PyRoot::g_true) return true;
        if (vp == PyRoot::g_false) return false;
        // 单例比不上就按 PyLong 布局退化解，热更挪了单例地址也还能用
        uint8_t b[32];
        if (!vm_readv(vp, b, 32)) return false;
        int64_t sz = 0; memcpy(&sz, b + 16, 8);
        uint32_t dg = 0; memcpy(&dg, b + 24, 4);
        return sz != 0 && dg != 0;
    }
    return false;
}

// 监管当前辅助特质的剩余冷却。链路见 idv-replay-genius 第四节：
//   unit.skill_mgr -> skill_dict{技能id: Skill}
//   Skill._cd_delta  = 剩余秒数，**不在冷却中时恒为 0**(所以 0 就是"就绪")
//   充能型(窥视者)：power_num=最大层数 _cur_power_num=当前层数 _cd_delta=距下一层回满
//
// ！！不能用 leader_skills + is_leader_skill 筛！！(旧实现就是这么写的，结果永远"就绪")
// leader_skills 里混着角色自身技能(红夫人 2601 普攻 lead=True、_cd_delta 恒 0)，
// is_leader_skill=True 的也不止一个(窥视者 25732/25733 都是，但只有 25732 在走)。
//
// 特质 -> 技能 id 来自游戏配表 GK.DM.support_skill_data[特质][handheld_id]['skill_id']
// (2026-09-22 注入 dump，原始数据存档在 逆向存档\2026.0917_b6f53566\captures\support_skill_data.txt)。
// 同一特质按监管角色(handheld_id)有不同的技能 id，而且**没有统一规律**：
//   传送 多数 x741/x743，但也有 12644、13718、3844……；移形每个角色都不同(8802、8811、8883……)
// 每组的**第一个 id 是主技能**，已实测：窥视者 732(733 的 _cd_delta 恒 30)、闪现 711、
// 传送 741(与 743 共享冷却)。这里把每个特质下所有角色的主技能 id 收成一个集合，
// 在 skill_dict 里找落在集合里的那个 —— 一个监管身上只挂自己那一版，不会撞。
// support_skill_id 是实时值(底牌换完立刻变)，旧特质那些冻结的技能对象不在当前集合里，自然不会被选中。
// ⚠️ 热更上新监管时这张表会缺新角色的 id：表现是该角色只显示特质名、不显示冷却，不会给错值。
//    重新 dump：注入 probe2，命令通道里读 GK.DM.support_skill_data。
static const int32_t MAIN_SKILL_LISTEN[]   = {701};
static const int32_t MAIN_SKILL_ABNORMAL[]   = {751};
static const int32_t MAIN_SKILL_EXCITEMENT[]   = {722};
static const int32_t MAIN_SKILL_PATROLLER[] = {761, 763, 765, 3850, 9761, 10761, 11761, 12650, 13450, 13724, 13761, 14050,
                                      14559, 14738, 15159, 15359, 15569, 17761, 21761, 22761, 23761, 24761, 25761,
                                      26761, 27761, 28761, 29761, 30761, 31761, 32761, 34761, 35761, 125750, 128750,
                                      136850, 143750};
static const int32_t MAIN_SKILL_TELEPORT[]   = {741, 3741, 3844, 8741, 9741, 10741, 12644, 13444, 13718, 13741, 14044, 14553,
                                      14732, 15153, 15353, 15563, 17741, 21741, 22741, 23741, 24741, 25741, 26741,
                                      27741, 28741, 29741, 30741, 31741, 32741, 34741, 35741, 125744, 128744, 136844,
                                      143744};
static const int32_t MAIN_SKILL_PEEPER[] = {732, 735, 738, 3841, 9732, 10732, 11732, 12641, 13441, 13715, 13732, 14041,
                                      14550, 14729, 15150, 15350, 15560, 17732, 21732, 22732, 23732, 24732, 25732,
                                      26732, 27732, 28732, 29732, 30732, 31732, 32732, 34732, 35732, 125741, 128741,
                                      136841, 143741};
static const int32_t MAIN_SKILL_BLINK[]   = {711};
static const int32_t MAIN_SKILL_SHIFT[]   = {8802, 8805, 8808, 8811, 8814, 8817, 8820, 8823, 8826, 8829, 8832, 8835, 8838,
                                      8841, 8844, 8847, 8850, 8853, 8856, 8859, 8862, 8865, 8868, 8871, 8874, 8877,
                                      8880, 8883, 8886, 8890, 8893, 8896, 14562, 14742, 15162, 15362, 15572, 143753};

#define PICK_TABLE(a) do { p = a; n = (int)(sizeof(a) / sizeof(a[0])); } while (0)
static bool is_trait_main_skill(int trait, int64_t sid)
{
    const int32_t *p = nullptr; int n = 0;
    switch (trait) {
        case 1: PICK_TABLE(MAIN_SKILL_LISTEN);   break;
        case 2: PICK_TABLE(MAIN_SKILL_ABNORMAL);   break;
        case 3: PICK_TABLE(MAIN_SKILL_EXCITEMENT);   break;
        case 4: PICK_TABLE(MAIN_SKILL_PATROLLER); break;
        case 5: PICK_TABLE(MAIN_SKILL_TELEPORT);   break;
        case 6: PICK_TABLE(MAIN_SKILL_PEEPER); break;
        case 7: PICK_TABLE(MAIN_SKILL_BLINK);   break;
        case 8: PICK_TABLE(MAIN_SKILL_SHIFT);   break;
        default: return false;
    }
    for (int i = 0; i < n; i++) if (p[i] == sid) return true;
    return false;
}

// unit.skill_mgr -> skill_dict 的条目表。监管和求生者共用。
// 速率 = skill_mgr.cd_rate：实测恒为 int 1，但名字说明它能变(加速冷却类效果)，读不到就按 1
static bool get_skill_dict(uint64_t d, const Dict &dk, Dict &sdk, float &rate)
{
    rate = 1.f;
    g_i_skillmgr = resolve_index(d, dk, g_i_skillmgr, "skill_mgr");
    if (g_i_skillmgr < 0) return false;
    uint64_t sm = getPtr64(value_slot(dk, g_i_skillmgr));
    if (!is_obj(sm)) return false;
    uint64_t smd = getPtr64(sm - 0x18);
    Dict smk;
    if (!read_dict(smd, smk)) return false;

    g_i_skilldict = resolve_index(smd, smk, g_i_skilldict, "skill_dict");
    if (g_i_skilldict < 0) return false;
    g_i_cdrate = resolve_index(smd, smk, g_i_cdrate, "cd_rate");
    if (g_i_cdrate >= 0 && !read_float(value_slot(smk, g_i_cdrate), rate, 0.01f, 10.f)) rate = 1.f;
    uint64_t sd = getPtr64(value_slot(smk, g_i_skilldict));
    return is_obj(sd) && getPtr64(sd + 8) == PyRoot::g_dict_type && read_dict(sd, sdk);
}

// 读一个 Skill 对象的冷却。_cd_delta 读不到返回 false(不显示冷却，不猜)，其余几项读不到就保持原值
static bool read_skill_cd(uint64_t sk, float &remain, float &total, int &charge_max, int &charge_cur)
{
    if (!is_obj(sk)) return false;
    uint64_t skd = getPtr64(sk - 0x18);
    Dict skk;
    if (!read_dict(skd, skk)) return false;

    g_i_cd       = resolve_index(skd, skk, g_i_cd,       "_cd_delta");
    g_i_cdtime   = resolve_index(skd, skk, g_i_cdtime,   "cd_time");
    g_i_power    = resolve_index(skd, skk, g_i_power,    "power_num");
    g_i_curpower = resolve_index(skd, skk, g_i_curpower, "_cur_power_num");
    if (g_i_cd < 0) return false;
    if (!read_float(value_slot(skk, g_i_cd), remain, -0.5f, 600.0f)) return false;

    if (g_i_cdtime >= 0) read_float(value_slot(skk, g_i_cdtime), total, 0.f, 600.f);
    int64_t v = 0;
    if (g_i_power    >= 0 && read_int(getPtr64(value_slot(skk, g_i_power)),    v) && v > 0 && v < 16) charge_max = (int)v;
    if (g_i_curpower >= 0 && read_int(getPtr64(value_slot(skk, g_i_curpower)), v) && v >= 0 && v < 16) charge_cur = (int)v;
    return true;
}

static void collect_hunter_skill(uint64_t d, const Dict &dk, int trait, Info &res)
{
    if (trait < 1 || trait > 8) return;
    Dict sdk; float rate;
    if (!get_skill_dict(d, dk, sdk, rate)) return;

    for (int64_t i = 0; i < sdk.n_entries; i++) {
        int64_t sid = 0;
        if (!read_int(getPtr64(key_slot(sdk, i)), sid)) continue;
        if (!is_trait_main_skill(trait, sid)) continue;          // 先按 id 筛，只解那一个技能对象
        float cd = 0.f;
        if (!read_skill_cd(getPtr64(value_slot(sdk, i)), cd, res.cd_total, res.charge_max, res.charge_cur)) return;
        res.cd_remain = cd;
        res.cd_read_ms = now_ms();
        res.cd_rate = rate;
        res.has_cd   = true;
        return;                                        // 一个监管只挂一个主技能
    }
}

// 求生者的飞轮效应冷却。skill_dict 里没有 11111 = 没带飞轮(或这一轮没读到)，什么都不填
static void collect_flywheel(uint64_t d, const Dict &dk, Info &res)
{
    Dict sdk; float rate;
    if (!get_skill_dict(d, dk, sdk, rate)) return;

    for (int64_t i = 0; i < sdk.n_entries; i++) {
        int64_t sid = 0;
        if (!read_int(getPtr64(key_slot(sdk, i)), sid) || sid != SKILL_FLYWHEEL) continue;
        float cd = 0.f, total = 0.f; int full = 0, cur = 0;
        if (!read_skill_cd(getPtr64(value_slot(sdk, i)), cd, total, full, cur)) return;
        res.flywheel_remain = cd;
        res.flywheel_read_ms = now_ms();
        res.flywheel_rate = rate;
        res.has_flywheel   = true;
        return;
    }
}

// ============================================================================
// 花名册 GK.g_avatar.player_data_list —— 大天赋的**权威来源**(2026-09-23 注入实测)
// ============================================================================
// 每条是一个 dict：{role_type(1监管/2求生), eid, pid, final_genius[终端天赋], support_skill_id,
//                   player_name, bot, cloth_id, is_ready, uid(真人才有)}
// 为什么改走这里：genius_id_lv_lst 是十几个嵌套 list 的冷页，内存压力下整片读不出(待办 #7)，
// 为它写了一整套"增量累积+二次确认"；而这份是账号级对象上的一层 list+dict，结构浅得多。
// 局内局外都在（准备阶段 UI 上那份 UIRoomWait.eid_2_player_info 进局就销毁，这份不会）。
//
// ⚠ 三件必须记住的事：
//   1) **final_genius 只有四个终端天赋(8/16/24/32)，不含 26(绝处逢生)** —— 26 仍然只能从
//      genius_id_lv_lst 读，所以那套记忆逻辑不能全删，只是现在只服务于这一个 bit。
//   2) **监管那条的 final_genius 恒为 None**(实测真人局)：天赋只对同阵营公开。
//      但 support_skill_id **是公开的**，求生者也能看到监管带什么特质。
//   3) 配对分两种：人机 unit.unique_id 是 int，对 eid；真人是账号字符串，对 uid。
static RosterEntry g_roster[MAX_ROSTER];   // 读线程的工作副本，填完拷进 g_roster_buf 发布
static RosterEntry g_civ[MAX_ROSTER];      // 准备阶段(我是监管)的求生者天赋，同样双缓冲发布
static int g_civ_n = 0;
static int g_roster_n = 0;
static int64_t g_i_avatar = -1, g_i_pdl = -1;
static int64_t g_i_role = -1, g_i_eid = -1, g_i_fg = -1, g_i_ruid = -1, g_i_rpid = -1;

static uint32_t bits_of_terminal(int64_t id)
{
    switch (id) {
        case 8:  return BIT_ID_8;
        case 16: return BIT_ID_16;
        case 24: return BIT_ID_24;
        case 26: return BIT_ID_26;
        case 32: return BIT_ID_32;
        default: return 0;
    }
}


static void read_roster()
{
    g_roster_n = 0;
    g_i_avatar = find_key(g_module_dict, "g_avatar", g_i_avatar);
    if (g_i_avatar < 0) return;
    uint64_t av = get_attr(g_module_dict, g_i_avatar);
    uint64_t ad = get_inst_dict(av);
    if (!ad) return;
    Dict adk;
    if (!read_dict(ad, adk)) return;
    g_i_pdl = resolve_index(ad, adk, g_i_pdl, "player_data_list");
    if (g_i_pdl < 0) return;

    int64_t n = 0; uint64_t items = 0;
    if (!read_list(getPtr64(value_slot(adk, g_i_pdl)), n, items) || n == 0) return;

    for (int64_t i = 0; i < n && g_roster_n < MAX_ROSTER; i++) {
        uint64_t e = getPtr64(items + i * 8);        // 每条是普通 dict，不是实例
        Dict ek;
        if (!read_dict(e, ek)) continue;
        RosterEntry r;
        memset(&r, 0, sizeof(r));
        r.eid = -1;

        g_i_role = resolve_index(e, ek, g_i_role, "role_type");
        int64_t v = 0;
        if (g_i_role < 0 || !read_int(getPtr64(value_slot(ek, g_i_role)), v)) continue;
        r.role_type = (int)v;

        g_i_rpid = resolve_index(e, ek, g_i_rpid, "pid");
        if (g_i_rpid >= 0 && read_int(getPtr64(value_slot(ek, g_i_rpid)), v)) r.pid = (int)v;

        // eid: 人机是 int，真人是 ObjectId(读不成 int，留 -1)
        g_i_eid = resolve_index(e, ek, g_i_eid, "eid");
        if (g_i_eid >= 0 && read_int(getPtr64(value_slot(ek, g_i_eid)), v)) r.eid = v;

        // uid: 真人才有，是账号 id 字符串(纯数字，紧凑 ASCII)
        g_i_ruid = resolve_index(e, ek, g_i_ruid, "uid");
        if (g_i_ruid >= 0) read_str(getPtr64(value_slot(ek, g_i_ruid)), r.uid, sizeof(r.uid));

        // final_genius: 终端天赋列表(监管恒为 None)
        g_i_fg = resolve_index(e, ek, g_i_fg, "final_genius");
        if (g_i_fg >= 0) {
            int64_t m = 0; uint64_t fit = 0;
            if (read_list(getPtr64(value_slot(ek, g_i_fg)), m, fit)) {
                for (int64_t j = 0; j < m && j < 8; j++) {
                    int64_t id = 0;
                    if (read_int(getPtr64(fit + j * 8), id)) r.bits |= bits_of_terminal(id);
                }
            }
        }

        g_roster[g_roster_n++] = r;
    }
}

// 准备阶段(本机是监管时)的求生者天赋：**GK.g_avatar.final_genius_dict** = {str(eid): [终端天赋...]}
//   2026-09-24 监管视角 60 秒差分(对面真人求生者 60 秒内换了 16 次天赋)：只有 final_genius_dict 每次都跟着变；
//   UIRoomWait.eid_2_player_info 和 player_data_list 从头到尾停在进房时的值(只有 status 在变) —— 都是快照。
//   c_skill_data 在我是监管时是空列表(它只装本阵营)。第一、二版分别用了 c_skill_data / eid_2_player_info，都不对。
// 键是字符串：人机 = str(int eid)(如 "1000")；真人 = ObjectId 的 24 位小写 hex。
//   ObjectId 布局(2026-09-24 实测)：basicsize 24、只有一个槽 __id → 对象 +0x10 是 bytes 对象，
//   bytes +0x10 = 长度(12)、+0x20 起 12 字节原始内容，转 hex 与 str(oid) 一致。
// 座位号：UIRoomWait.uid2pos = {eid 对象: 座位 1..4}，只含求生者(本机监管在 b_uid2pos)。按座位逐个拼键去查。
// 本机阵营：UIRoomWait.role，1 = 我是监管、2 = 我是求生者(两个阵营各实测过)。只有 role == 1 才收集。
//   UIRoomWait 进局就销毁；final_genius_dict 在大厅里是上一局的数据，所以必须以面板存在为前提。
// ⚠ 不能用 find_key 按这些动态字符串查：它的驻留缓存会**保存 name 指针**(只能传字符串常量)。
static int64_t g_i_uimgr = -1, g_i_uidict = -1, g_i_prole = -1, g_i_uid2pos = -1, g_i_fgd = -1;
static bool g_room_wait = false;   // 本轮找到了 UIRoomWait 面板(只给读线程自己用，见 try_refresh)

// eid 对象 -> final_genius_dict 的键：int 转十进制；ObjectId 取 12 字节转 24 位小写 hex(布局见上)
static bool eid_key_string(uint64_t eid_obj, char *out, size_t cap)
{
    int64_t iv = 0;
    if (read_int(eid_obj, iv)) { snprintf(out, cap, "%lld", (long long)iv); return true; }
    if (!is_obj(eid_obj) || cap < 25) return false;
    uint64_t b = getPtr64(eid_obj + 0x10);
    if (!is_obj(b)) return false;
    int64_t n = 0;
    uint8_t raw[12];
    if (!vm_readv(b + 0x10, &n, 8) || n != 12 || !vm_readv(b + 0x20, raw, 12)) return false;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 12; i++) { out[i * 2] = hx[raw[i] >> 4]; out[i * 2 + 1] = hx[raw[i] & 15]; }
    out[24] = 0;
    return true;
}

// ---- 座位缓存：每个房间只建一次 "座位 -> final_genius_dict 的键"，之后每轮只读 final_genius_dict ----
// 2026-09-24 实测(VmSwap 1.1G > VmRSS 675M)：uid2pos 里的 eid 对象(ObjectId / int)是进房时建的冷页，
// 几分钟后就被换进 zram，驱动读不到 -> 每轮现拼键就全部失败 -> "求生者天赋0条"。
// 而 final_genius_dict 每次有人换天赋都会被游戏改写，一直是热的。所以：
//   - 键字符串一个房间内不变，建好就缓存(面板对象换了 = 换房间，清空)
//   - 还缓存 final_genius_dict 里匹配到的键对象指针，下一轮先比指针，连键字符串都不用读
//   - 某轮读不到就沿用上一次读到的天赋，不清空(读到了就覆盖，包括读到空列表 —— 对面换天赋途中会短暂是 [])
struct SeatCache { int seat; char key[32]; uint64_t key_obj; uint32_t bits; };
// 联合狩猎 8 个求生者(2026-10-25 从 4 扩到 8，座位号 1~8，用户上机确认)
static const int MAX_SEAT = 8;
static SeatCache g_seat[MAX_SEAT];
static int g_seat_n = 0;
static uint64_t g_seat_panel = 0;      // 建缓存时的 UIRoomWait 实例
static uint64_t g_seat_src = 0;        // 建缓存时的 uid2pos dict；全部座位都建成功才记下，否则下一轮继续补

static SeatCache *seat_slot(int seat)
{
    for (int i = 0; i < g_seat_n; i++) if (g_seat[i].seat == seat) return &g_seat[i];
    if (g_seat_n >= MAX_SEAT) return nullptr;
    SeatCache &c = g_seat[g_seat_n++];
    memset(&c, 0, sizeof(c));
    c.seat = seat;
    return &c;
}

static void read_civilian_skill_rows()
{
    g_civ_n = 0;      // ⚠ 必须在最前面清：下面有多个提前 return(进局后面板就销毁了)，
                      //    放到循环前面会让局内残留上一次准备阶段的值
    g_room_wait = false;
    g_prep_as_hunter = false;
    g_i_uimgr = find_key(g_module_dict, "g_ui_mgr", g_i_uimgr);
    if (g_i_uimgr < 0) return;
    uint64_t um = get_attr(g_module_dict, g_i_uimgr);
    uint64_t ud = get_inst_dict(um);
    if (!ud) return;
    Dict udk;
    if (!read_dict(ud, udk)) return;
    g_i_uidict = resolve_index(ud, udk, g_i_uidict, "ui_dict");
    if (g_i_uidict < 0) return;
    uint64_t uid_dict = getPtr64(value_slot(udk, g_i_uidict));
    int64_t i_panel = find_key(uid_dict, "UIRoomWait", -1);      // 面板键就是类名字符串
    if (i_panel < 0) return;                                     // 不在准备界面
    uint64_t panel = get_attr(uid_dict, i_panel);
    uint64_t pd = get_inst_dict(panel);
    if (!pd) return;
    Dict pdk;
    if (!read_dict(pd, pdk)) return;
    g_room_wait = true;
    if (panel != g_seat_panel) { g_seat_panel = panel; g_seat_n = 0; g_seat_src = 0; }   // 换房间

    int64_t v = 0;
    g_i_prole = resolve_index(pd, pdk, g_i_prole, "role");
    if (g_i_prole < 0 || !read_int(getPtr64(value_slot(pdk, g_i_prole)), v) || v != BUTCHER_UNIT_TYPE) return;
    g_prep_as_hunter = true;

    // 一、座位缓存：uid2pos 变了(有人进出)或上次没建全，就(再)建一次。读不到的座位留到下一轮补
    g_i_uid2pos = resolve_index(pd, pdk, g_i_uid2pos, "uid2pos");
    uint64_t src = (g_i_uid2pos >= 0) ? getPtr64(value_slot(pdk, g_i_uid2pos)) : 0;
    Dict sk;
    if (src && src != g_seat_src && read_dict(src, sk)) {
        int want = 0, ok = 0;
        for (int64_t i = 0; i < sk.n_entries; i++) {
            uint64_t kp = getPtr64(key_slot(sk, i));
            if (kp == 0) continue;                                   // 已删除的条目
            want++;
            if (!read_int(getPtr64(value_slot(sk, i)), v) || v < 1 || v > MAX_SEAT) continue;
            char key[32];
            if (!eid_key_string(kp, key, sizeof(key))) continue;
            SeatCache *c = seat_slot((int)v);
            if (!c) continue;
            if (strcmp(c->key, key) != 0) {                          // 这个座位换人了
                snprintf(c->key, sizeof(c->key), "%s", key);
                c->key_obj = 0; c->bits = 0;
            }
            ok++;
        }
        if (want > 0 && ok == want) g_seat_src = src;
    }
    if (g_seat_n == 0) return;

    // 二、每轮只读 final_genius_dict(read_roster() 已在本轮先解析过 g_i_avatar)。读不到就沿用缓存
    Dict fk;
    bool fk_ok = false;
    if (g_i_avatar >= 0) {
        uint64_t ad = get_inst_dict(get_attr(g_module_dict, g_i_avatar));
        Dict adk;
        if (ad && read_dict(ad, adk)) {
            g_i_fgd = resolve_index(ad, adk, g_i_fgd, "final_genius_dict");
            fk_ok = g_i_fgd >= 0 && read_dict(getPtr64(value_slot(adk, g_i_fgd)), fk);
        }
    }
    for (int s = 0; fk_ok && s < g_seat_n; s++) {
        SeatCache &c = g_seat[s];
        int64_t hit = -1;
        for (int64_t i = 0; i < fk.n_entries && hit < 0; i++) {      // 先比缓存的键对象指针：不用读字符串
            uint64_t kp = getPtr64(key_slot(fk, i));
            if (kp && kp == c.key_obj) hit = i;
        }
        for (int64_t i = 0; i < fk.n_entries && hit < 0; i++) {
            uint64_t kp = getPtr64(key_slot(fk, i));
            if (str_equals(kp, c.key)) { hit = i; c.key_obj = kp; }
        }
        if (hit < 0) continue;                                       // 没找到(也可能是键字符串页读不到)：沿用
        int64_t m = 0; uint64_t git = 0;
        if (!read_list(getPtr64(value_slot(fk, hit)), m, git)) continue;   // 值读不到：沿用
        uint32_t bits = 0;
        bool all = true;
        for (int64_t j = 0; j < m && j < 8; j++) {
            if (read_int(getPtr64(git + j * 8), v)) bits |= bits_of_terminal(v);
            else all = false;
        }
        if (all) c.bits = bits;                                      // 读全了才覆盖(含空列表)
    }

    // 三、按座位号发布
    for (int s = 0; s < g_seat_n && g_civ_n < MAX_ROSTER; s++) {
        if (g_seat[s].bits == 0) continue;
        RosterEntry e;
        memset(&e, 0, sizeof(e));
        e.eid = -1;
        e.role_type = CIVILIAN_UNIT_TYPE;
        e.seat = g_seat[s].seat;
        e.bits = g_seat[s].bits;
        int k = g_civ_n++;
        while (k > 0 && g_civ[k - 1].seat > e.seat) { g_civ[k] = g_civ[k - 1]; k--; }
        g_civ[k] = e;
    }
}

// 按单位的 unique_id 找花名册条目：人机是 int(对 eid)，真人是账号字符串(对 uid)
static const RosterEntry *roster_find(uint64_t unique_slot)
{
    if (g_roster_n == 0) return nullptr;
    uint64_t vp = getPtr64(unique_slot);
    int64_t iv = 0;
    if (read_int(vp, iv)) {
        for (int i = 0; i < g_roster_n; i++)
            if (g_roster[i].eid == iv) return &g_roster[i];
        return nullptr;
    }
    char s[24];
    if (!read_str(vp, s, sizeof(s))) return nullptr;
    for (int i = 0; i < g_roster_n; i++)
        if (g_roster[i].uid[0] && strcmp(g_roster[i].uid, s) == 0) return &g_roster[i];
    return nullptr;
}

static int collect_type(uint64_t ubt, int type, int write_idx, int count)
{
    uint64_t lst = get_type_list(ubt, type);
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lst, n, items) || n == 0) return count;

    for (int64_t i = 0; i < n && count < MAX_UNITS; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        if (!is_obj(inst)) continue;
        uint64_t d = getPtr64(inst - 0x18);           // managed-dict 在对象前 0x18
        Dict dk;
        if (!read_dict(d, dk)) continue;

        const bool is_follower = (type == YIDHRA_PUPPET_UNIT_TYPE);
        g_i_support = resolve_index(d, dk, g_i_support, "support_skill_id");
        g_i_model   = resolve_index(d, dk, g_i_model,   "model");
        g_i_pos     = resolve_index(d, dk, g_i_pos,     "position");

        // 大天赋优先走花名册(见 read_roster())：结构浅、不受冷页换出影响
        const RosterEntry *rt_entry = nullptr;
        if (!is_follower) {
            g_i_unique = resolve_index(d, dk, g_i_unique, "unique_id");
            if (g_i_unique >= 0) rt_entry = roster_find(value_slot(dk, g_i_unique));
        }

        GeniusMemory *mem = nullptr;
        // 逐单位的 genius_id_lv_lst 是天赋的权威来源(实时、且含 26 绝处逢生 —— 26 不在 final_genius 里)
        if (!is_follower) {
            g_i_genius = resolve_index(d, dk, g_i_genius, "genius_id_lv_lst");
            g_i_uid    = resolve_index(d, dk, g_i_uid,    "uid");
            // 只在求生者类上找 ability_used：监管类上没这个属性，每个单位白扫一遍上千条不值
            if (type == CIVILIAN_UNIT_TYPE)
                g_i_ability = resolve_index(d, dk, g_i_ability, "ability_used");
            if (g_i_genius < 0) continue;

            int64_t uid = 0;
            bool has_uid = (g_i_uid >= 0) && read_int(getPtr64(value_slot(dk, g_i_uid)), uid);

            // 天赋按 uid 增量累积(见 read_genius())。这一轮 uid 读不到时天赋状态记为未知(只影响这一轮的显示)，
            // 记忆本身不动，下一轮读到 uid 会接着用
            mem = has_uid ? get_mem(uid) : nullptr;
            if (mem) read_genius(value_slot(dk, g_i_genius), *mem);
        }

        int trait = 0;
        if (g_i_support >= 0) {
            uint64_t sp = getPtr64(value_slot(dk, g_i_support));
            int64_t m = 0; uint64_t sit = 0;
            if (read_list(sp, m, sit) && m >= 1) {
                int64_t v = 0;
                if (read_int(getPtr64(sit), v) && v >= 1 && v <= 8) trait = (int)v;
            }
        }

        uint64_t scene = 0;
        if (g_i_model >= 0) {
            uint64_t mo = getPtr64(value_slot(dk, g_i_model));
            if (is_obj(mo)) {
                uint64_t sp2 = getPtr64(mo + 0x20);
                if (is_obj(sp2)) scene = sp2;
            }
        }
        float xyz[3] = {0, 0, 0};
        if (g_i_pos >= 0) {
            uint64_t pos = getPtr64(value_slot(dk, g_i_pos));
            if (is_obj(pos)) vm_readv(pos + 0x10, xyz, 12);
        }

        g_buf[write_idx][count].scene_obj = scene;
        g_buf[write_idx][count].x = xyz[0];
        g_buf[write_idx][count].y = xyz[2];
        bool used = false;
        if (type == CIVILIAN_UNIT_TYPE && g_i_ability >= 0)
            used = read_desperate_used(value_slot(dk, g_i_ability));

        Info inf;
        memset(&inf, 0, sizeof(inf));              // 冷却那几项求生者侧不填，必须先清零
        inf.camp     = type;
        inf.support_trait = trait;
        inf.desperate_used = used;
        if (mem) {
            inf.genius_bits   = mem_bits(*mem);
            inf.genius_state = mem_state(*mem);
            memcpy(inf.genius_levels, mem->levels, sizeof(inf.genius_levels));
        }
        // 花名册只作**兜底**：它的 final_genius 是进房时的快照，玩家进局前改了天赋它不会更新
        // (2026-09-23 实测：准备界面把天赋从 [16,32] 改成 [8,24]，player_data_list 纹丝不动，
        //  只有 g_avatar.final_genius_dict 和 UIRoomWait.c_skill_data 跟着变)。
        // 所以逐单位的 genius_id_lv_lst 仍然是权威；只有它一条都没读出来时，才用快照顶一下，
        // 免得内存压力下天赋行整个空掉。**特质绝不用快照**(同样会滞后)，只认 unit.support_skill_id。
        if (rt_entry && inf.genius_state == GENIUS_UNKNOWN && rt_entry->bits) {
            inf.genius_bits |= rt_entry->bits;
            inf.genius_state = GENIUS_PARTIAL;      // 标成"不完整"，显示时带 ?，提醒这是兜底值
        }
        if (type == BUTCHER_UNIT_TYPE || is_follower) collect_hunter_skill(d, dk, trait, inf);
        else                                  collect_flywheel(d, dk, inf);

        g_buf[write_idx][count].info = inf;
        count++;
    }
    return count;
}

static bool try_refresh()
{
    if (g_module_dict == 0 && !resolve_module()) return false;

    // 花名册先读：局内用来给单位配大天赋，局外(准备阶段)是唯一的数据源。
    // 它和 units 无关，所以即使下面 unit_mgr 无效(局外)也要先读、先发布
    read_roster();                 // player_data_list：身份 + 监管特质(进房快照)
    read_civilian_skill_rows();    // g_avatar.final_genius_dict：求生者天赋(我是监管、有 UIRoomWait 时)
    {
        int w = 1 - g_active;
        for (int i = 0; i < g_roster_n; i++) g_roster_buf[w][i] = g_roster[i];
        g_roster_cnt[w] = g_roster_n;
        for (int i = 0; i < g_civ_n; i++) g_civ_buf[w][i] = g_civ[i];
        g_civ_cnt[w] = g_civ_n;
    }

    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? get_attr(g_module_dict, g_i_unit_mgr) : 0;
    if (!is_obj(um)) {
        // 局外：没有单位，但花名册有内容就发布出去(准备阶段显示监管特质/求生者天赋靠它)
        if (g_roster_n > 0 || g_civ_n > 0) {
            int w = 1 - g_active;
            g_count[w] = 0;
            g_active = w;
            snprintf(g_status, sizeof(g_status), T("局外 花名册%d人 实时天赋%d条", "out of match: roster %d, live talents %d"), g_roster_n, g_civ_n);
            return true;
        }
        snprintf(g_status, sizeof(g_status), "%s", T("unit_mgr 无效(未在对局中?)", "unit_mgr invalid (not in match?)"));
        return false;
    }

    // 换局了：uid 会从 1000001 重新开始，上一局的天赋记忆必须作废，否则会张冠李戴。
    // 注意只在**拿到有效新地址**时比较；um 无效时不动记忆，免得对局中途的
    // 短暂读失败把记忆误清。
    if (g_last_unit_mgr != 0 && um != g_last_unit_mgr) g_mem_count = 0;
    g_last_unit_mgr = um;

    uint64_t ud = getPtr64(um - 0x18);
    g_i_ubt = find_key(ud, "units_by_type", g_i_ubt);
    uint64_t ubt = (g_i_ubt >= 0) ? get_attr(ud, g_i_ubt) : 0;
    if (!is_obj(ubt)) { snprintf(g_status, sizeof(g_status), "%s", T("units_by_type 无效", "units_by_type invalid")); return false; }

    int write_idx = 1 - g_active, count = 0;
    // 监管和求生者的属性序号缓存是共用的；两个类的插入顺序不同，
    // 所以每个单位都会走一次"按名字校验，不对就重扫"，多花的时间可以忽略
    count = collect_type(ubt, BUTCHER_UNIT_TYPE,  write_idx, count);
    count = collect_type(ubt, CIVILIAN_UNIT_TYPE, write_idx, count);
    count = collect_type(ubt, YIDHRA_PUPPET_UNIT_TYPE, write_idx, count);   // 没有女巫时这个键不存在，直接返回

    if (count == 0) {
        // 准备阶段：unit_mgr 是存在的(大厅/准备阶段 units_by_type 有 69/100/1007 等键，2026-09-24 实测)，
        // 所以会走到这里而不是上面的 !is_obj(um) 分支。以前在这里直接 return false、不翻转缓冲，
        // 结果准备阶段的天赋读到了却从没发布，绘制侧一直读上一局局内那份 —— 准备阶段不显示求生者天赋的根因。
        // 有 UIRoomWait 面板就确定是准备阶段，照样发布；没有面板(局内中途读失败)仍保留旧缓冲。
        if (g_room_wait) {
            g_count[write_idx] = 0;
            g_active = write_idx;
            snprintf(g_status, sizeof(g_status), T("准备阶段 %s 求生者天赋%d条", "prep: %s, survivor talents %d"),
                     g_prep_as_hunter ? T("我是监管", "I'm hunter") : T("我是求生者(不显示)", "I'm survivor (hidden)"), g_civ_n);
            return true;
        }
        snprintf(g_status, sizeof(g_status), "%s", T("一个单位也没读出", "no unit read"));
        return false;
    }
    g_count[write_idx] = count;
    g_active = write_idx;
    snprintf(g_status, sizeof(g_status), T("正常 %d 人", "OK %d players"), count);
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
    if (try_refresh()) { g_available = true; g_fail_streak = 0; return; }
    g_available = false;
    if (++g_fail_streak >= 8) {                       // 连续失败就把序号缓存全部作废，下一轮重新按名字找
        g_module_dict = 0;
        g_i_unit_mgr = g_i_ubt = -1;
        g_i_genius = g_i_support = g_i_model = g_i_pos = g_i_uid = g_i_ability = -1;
        g_i_skillmgr = g_i_skilldict = g_i_cdrate = -1;
        g_i_unique = g_i_avatar = g_i_pdl = -1;
        g_i_role = g_i_eid = g_i_fg = g_i_ruid = g_i_rpid = -1;
        g_i_uimgr = g_i_uidict = g_i_prole = g_i_uid2pos = g_i_fgd = -1;
        g_i_cd = g_i_cdtime = g_i_power = g_i_curpower = -1;
        g_fail_streak = 0;
        // 这里**不清天赋记忆**：对局中途链路短暂中断也会走到这里，清了会让天赋行白白消失。
        // 记忆只在 unit_mgr 地址变化(换局)时清，见 try_refresh()
    }
}

// 天赋一局之内基本不变(只有监管带底牌换特质时会动)，不用像进度那样 100ms 一轮
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
    g_libbase   = libbase;
    snprintf(g_status, sizeof(g_status), "%s", T("启动中", "starting"));
    std::thread(thread_main).detach();
    printf("[天赋] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool available() { return g_available; }
static inline const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

// ---- 花名册：局内给单位配大天赋的兜底(见 collect_type)，局外不再直接拿来显示 ----
// (大厅里它仍是上一局的数据，2026-09-24 实测；准备阶段显示改走下面的 civ_*)
static inline int roster_size() { return g_roster_cnt[g_active]; }

// 准备阶段的求生者天赋(来自 final_genius_dict，实时)，已按座位号排好序。
// 只有本机是监管(prep_as_hunter())时才有内容；局内这里是 0 条，天赋走 lookup()
static inline int civ_size() { return g_civ_cnt[g_active]; }
static inline bool prep_as_hunter() { return g_prep_as_hunter; }

// seat：座位号 1..4，0 = 没配上
static bool civ_at(int i, int &seat, int &pid, uint32_t &bits)
{
    int b = g_active;
    if (i < 0 || i >= g_civ_cnt[b]) return false;
    seat = g_civ_buf[b][i].seat; pid = g_civ_buf[b][i].pid; bits = g_civ_buf[b][i].bits;
    return true;
}

// 配对规则跟 PyProgress.h 一致：先用 model+0x20 的指针身份，读不到再退回坐标近邻。
// 人物是移动的，坐标兜底给的阈值比密码机那边紧，宁可配不上也不要配错。
static bool lookup(uintptr_t obj, float x, float y, Info &out)
{
    if (!g_available) return false;
    int b = g_active, n = g_count[b];
    for (int i = 0; i < n; i++) {
        if (g_buf[b][i].scene_obj == (uint64_t)obj) { out = g_buf[b][i].info; return true; }
    }
    int nearest = -1; float min_dist = 1.0f;
    for (int i = 0; i < n; i++) {
        if (g_buf[b][i].scene_obj != 0) continue;
        if (g_buf[b][i].x == 0.f && g_buf[b][i].y == 0.f) continue;
        float d = fabsf(g_buf[b][i].x - x) + fabsf(g_buf[b][i].y - y);
        if (d < min_dist) { min_dist = d; nearest = i; }
    }
    if (nearest < 0) return false;
    out = g_buf[b][nearest].info;
    return true;
}

// 绝处逢生只在求生者侧成立：id 26 在监管侧是另一个天赋，按 id 直接判会误标
static inline bool has_desperate(const Info &g)
{
    return g.camp == CIVILIAN_UNIT_TYPE && (g.genius_bits & BIT_ID_26);
}

// 绝处逢生四态：没带 / 带了还没用 / 带了已经用掉 / 还不知道
// **"没带"只有在天赋表完整时才能断言**；不完整又还没确认到 26，就是"未知"，不能当成没带
enum { DESPERATE_NONE = 0, DESPERATE_AVAILABLE = 1, DESPERATE_USED = 2, DESPERATE_UNKNOWN = 3 };
static inline int desperate_state(const Info &g)
{
    if (has_desperate(g)) return g.desperate_used ? DESPERATE_USED : DESPERATE_AVAILABLE;
    return g.genius_state == GENIUS_COMPLETE ? DESPERATE_NONE : DESPERATE_UNKNOWN;
}

// 监管辅助特质那一行的完整文本：特质名 + 冷却/充能。
//   普通技能   "闪现 12.7s" / "闪现 就绪"
//   充能型技能 "窥视者 2/3 20.9s"(还在攒下一层) / "窥视者 3/3"(满层)
// _cd_delta 不在冷却中时恒为 0，所以 0 直接当"就绪"用，不需要额外的标志位。
static void trait_line_text(const Info &g, const char *name, char *buf, size_t cap);

// 辅助特质名。1~8 连号，全表已确认。英文取国际服 TID_NAME_*(离线解包，见解包存档 SKILL §7.8)
static const char *trait_name(int id)
{
    switch (id) {
        case 1: return T("聆听", "Listen");
        case 2: return T("失常", "Abnormal");
        case 3: return T("兴奋", "Excitement");
        case 4: return T("巡视者", "Patroller");
        case 5: return T("传送", "Teleport");
        case 6: return T("窥视者", "Peeper");
        case 7: return T("闪现", "Blink");
        case 8: return T("移形", "Warp");
        default: return "";
    }
}

// 读线程 500ms 才读一次 _cd_delta，直接显示会一卡一卡地跳 0.5。
// _cd_delta 实测严格按每秒 -1(乘 cd_rate)递减，所以绘制时从"读到的值"按经过的时间往下推。
// 推到 0 以下只停在 0.0，**不自己判就绪** —— 就绪以真正读到的 0 为准(见 cd_ready())，
// 否则底牌改写冷却、或者推算略快时会误报。下一轮读取会覆盖推算值，误差不超过一个读取周期。
static float extrapolate(float read_val, int64_t at_ms, float rate)
{
    if (read_val <= 0.05f) return 0.f;
    if (rate <= 0.f) rate = 1.f;
    float r = read_val - (float)(now_ms() - at_ms) / 1000.f * rate;
    return r > 0.f ? r : 0.f;
}
static float cd_remain_now(const Info &g)
{
    return g.has_cd ? extrapolate(g.cd_remain, g.cd_read_ms, g.cd_rate) : 0.f;
}

// 就绪 = 真正读到 0(不是推算到 0)
static inline bool cd_ready(const Info &g) { return g.has_cd && g.cd_remain <= 0.05f; }
static inline bool flywheel_ready(const Info &g) { return g.has_flywheel && g.flywheel_remain <= 0.05f; }

// 天赋简称行，拆成 前 / 飞轮 / 后 三段，飞轮单独一段好让绘制侧单独上色。
// 求生者：双弹 [飞轮] 搏命 大心脏(固定最后)；监管全部在"前"段：封窗 底牌 张狂 挽留(固定最后)。
//   未知：三段全空(不画)
//   部分：已确认的大天赋 + "?"，表示可能还有没读到的
//   完整：全部大天赋；一个大天赋都没带就写"无"
// 飞轮段：就绪 "飞轮"(飞轮就绪=true，画红) / 冷却中 "飞轮13s" / 没读到冷却 "飞轮"。
// skill_dict 里有飞轮技能本身就证明带了飞轮，所以天赋表读不全(未知/部分)时也照样显示它。
// 英文用英文社区通用缩写(KJR/FW/TT/BT、CS/TC/Ins/Det)，每个词后带一个空格，三段直接拼起来也分得开。
struct GeniusLine { char pre[48]; char flywheel[24]; char post[48]; bool flywheel_ready; };
static inline void seg_add(char *buf, size_t cap, const char *cn, const char *en)
{
    strncat(buf, T(cn, en), cap - strlen(buf) - 1);
}
static void genius_segments(const Info &g, GeniusLine &o)
{
    memset(&o, 0, sizeof(o));
    bool is_survivor = (g.camp == CIVILIAN_UNIT_TYPE);
    bool carries_flywheel = is_survivor && ((g.genius_bits & BIT_ID_8) || g.has_flywheel);
    if (g.genius_state == GENIUS_UNKNOWN && !carries_flywheel) return;
    if (is_survivor) {
        if (g.genius_bits & BIT_ID_32) seg_add(o.pre, sizeof(o.pre), "双弹", "KJR ");
        if (carries_flywheel) {
            const char *fw = T("飞轮", "FW");
            if (flywheel_ready(g)) { snprintf(o.flywheel, sizeof(o.flywheel), "%s%s", fw, g_english ? " " : ""); o.flywheel_ready = true; }
            else if (g.has_flywheel) snprintf(o.flywheel, sizeof(o.flywheel), g_english ? "%s%.0fs " : "%s%.0fs", fw, extrapolate(g.flywheel_remain, g.flywheel_read_ms, g.flywheel_rate));
            else               snprintf(o.flywheel, sizeof(o.flywheel), "%s%s", fw, g_english ? " " : "");
        }
        if (g.genius_bits & BIT_ID_24) seg_add(o.post, sizeof(o.post), "搏命", "TT ");
        if (g.genius_bits & BIT_ID_16) seg_add(o.post, sizeof(o.post), "大心脏", "BT ");
    } else {
        if (g.genius_bits & BIT_ID_8)  seg_add(o.pre, sizeof(o.pre), "封窗", "CS ");
        if (g.genius_bits & BIT_ID_16) seg_add(o.pre, sizeof(o.pre), "底牌", "TC ");
        if (g.genius_bits & BIT_ID_32) seg_add(o.pre, sizeof(o.pre), "张狂", "Ins ");
        if (g.genius_bits & BIT_ID_24) seg_add(o.pre, sizeof(o.pre), "挽留", "Det ");
    }
    if (g.genius_state != GENIUS_COMPLETE) strncat(o.post, "?", sizeof(o.post) - strlen(o.post) - 1);
    else if (!o.pre[0] && !o.flywheel[0] && !o.post[0]) snprintf(o.pre, sizeof(o.pre), "%s", T("无", "None"));
}

static void trait_line_text(const Info &g, const char *name, char *buf, size_t cap)
{
    if (!name || !name[0]) { buf[0] = '\0'; return; }
    if (!g.has_cd) { snprintf(buf, cap, "%s", name); return; }   // 读不到冷却就只写名字
    if (g.charge_max > 0) {                                      // 充能型
        // 满层时 _cd_delta 的表现没验过，按读到的层数判满层；层数本身不推算，等下一轮读取
        if (g.charge_cur >= g.charge_max) snprintf(buf, cap, "%s %d/%d", name, g.charge_cur, g.charge_max);
        else snprintf(buf, cap, "%s %d/%d %.1fs", name, g.charge_cur, g.charge_max, cd_remain_now(g));
        return;
    }
    if (cd_ready(g)) snprintf(buf, cap, "%s %s", name, T("就绪", "ready"));
    else           snprintf(buf, cap, "%s %.1fs", name, cd_remain_now(g));
}

} // namespace 天赋

#endif // IDV_PY_GENIUS_H
