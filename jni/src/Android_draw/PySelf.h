#ifndef IDV_PY_SELF_H
#define IDV_PY_SELF_H

// ============================================================================
// 自身锚点 + 废弃模型黑名单 —— 走游戏内嵌 CPython 的对象图读取
// ============================================================================
//
// 解决两件一直靠启发式凑的事：
//
// 一、"当前操控的是谁"
//   旧做法是 draw_Gui.cpp 里那套 `相机深度(10~40) + zy + 阵营是1或2`。
//   它的问题：任何一个求生者走进 10~40 这条深度带就会被误判成自身，而 +0xaa(zy)
//   按实测是通用状态位、**不区分是否自身**。自身锚错 -> Z 锚错 -> 全场距离全错，
//   而且那个对象会被 continue 掉不再绘制。
//
//   ★ 引擎自己持有答案：GK.g_cam_ctrl.unit 就是"当前视角/操控的单位"。
//     2026-09-22 受控实验，两角色两阵营各切换一次共 4 次读数，4/4 全部指对：
//       求生者机械师  本体 ⇄ 机械玩偶(MyCivilianPuppetUnit, 在 units_by_type[2])
//       监管梦之女巫  本体 ⇄ 梦之信徒(MyYidhraPuppetUnit,   在 units_by_type[236])
//
//   ！！`GK.g_unit` 不能用 ！！它是"我的主角色"，切到从属时纹丝不动(4/4 从不跟随)。
//   拿它做自身高亮，机械师操控玩偶 / 女巫操控信徒时会高亮错人。这是个陷阱，
//   曾经的记录"g_unit = 本机玩家"在能切换操控对象的角色上就是错的。
//   同理 `g_cam_ctrl.free_mode_unit` 恒为主角色，也不是我们要的。
//
//   另外这两个字段**不要碰**：
//     is_operating     机械师挂在本体上，语义跟名字相反("本体正在遥控玩偶中")，
//                      而且每个角色名字都不一样(女巫叫 yidhra_operating)，没通用性
//     bj_operating     所有单位(含 AI)恒为 True，完全没用
//
// 二、"废弃模型"
//   形态切换类角色(红蝶等)换形态**不是换操控对象**(cam.unit 不变)，是同一个 unit
//   换模型。旧形态的场景对象留在数组里、仍然被绘制循环捕获，这就是"废弃模型"。
//
//   ★ 判据：unit.another_model + 0x20 就是废弃形态的场景对象指针。
//     实测红蝶一局 model+0x20 = 0x7ACBDFFF80(在画的那个)、
//     another_model+0x20 = 0x7ACDDD0600，后者与独立抓到的废弃对象地址逐位相同。
//   这替掉了 ShouldSkipEntity 里那份红蝶/木偶师类名黑名单：精确、跟视角无关、不用维护名单。
//
//   ⚠️ 不要拿 `+0x73`/`+0x70`(= NeoX 模型对象的 visible) 当过滤器。
//   它是渲染剔除的结果：废弃模型恒不可见，但**远处的真实对象同样不可见**
//   (实测手持飞刀这种真道具 +0x70 也是 0)。用它过滤会重演"求生者只看得到身边的密码机"。
//
//   ⚠️ 只覆盖 another_model 这一类。实测另外还有 balloon_model(女巫气球)、
//   _fragrance_image(木偶师忘忧之香残影) 等同样指向非活跃模型的属性，它们目前靠
//   ShouldSkipEntity 的鬼魂判定挡住。要做全的话，应当取 unit.model 的 ob_type
//   拿到 world.model 类型指针，再扫整个实例 dict 把该类型的值全部拉黑(只留 model 那份)，
//   并把扫出来的序号按对局缓存住，否则每轮扫上千个条目开销太大。
//
// 链路、CPython 3.11 布局、三条铁律跟 PyProgress.h 完全同源，那边文件头有详细说明。
// 这里按同样风格自带一份 dict 工具，免得去改那两个已经验证过的文件。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <thread>
#include <atomic>
#include <unistd.h>
#include "PyRoot.h"
#include "PyCore.h"

// 命名空间跟文件同名(PySelf)；draw_Gui.cpp 里另有全局变量 `uintptr_t self_obj`，别起成 self_obj 之类重名
namespace PySelf {

// sys.modules 与 dict/int/True 由 PyRoot.h 运行时找出，这里不再写死模块偏移

static const int MAX_STALE = 32;
static const int MAX_BODIES = 32;

// 要扫的单位类型：1=监管(巡视者也在这里) 2=求生者(机械玩偶、幻灯师分身也在这里) 236=梦之信徒
// 1065=伊斯人幻影(yith_ghost03，2026-09-22 实测；漏了它时幻影被本体集合挡掉)。
// 局中召出来的从属单位常有独立的键，某个召唤物被误挡时先用 _scripts\cmd_phase.py 查它在哪个键下
static const int UNIT_TYPES[] = {1, 2, 236, 1065};
static const int UNIT_TYPE_COUNT = sizeof(UNIT_TYPES) / sizeof(UNIT_TYPES[0]);

// ---- 本体集合：哪些场景对象是"角色本体"(2026-09-22 实测) ----
// 场景数组里跟角色同名、或者挂在角色身上的对象很多：_fragrance_image(每个求生者一份、同名、恒不可见)、
// another_model(另一形态)、balloon_model/umbrella_model(挂件)、item_lst[i].model(手持道具)、
// 时装挂件、约瑟夫相机……类名和 +0x240/+0x6D 都分不开(另一形态、气球跟本体一样是 2 / 0x50)。
// 唯一干净的定义：单位类型表里各键下每个单位的 unit.model + 0x20。
//   - 魔术师分身是 CloneUnit，在独立的键 [17] 里，不在这张表里，自然排除
//   - 幻灯师分身 SlideManCloneUnit 却在 [2] 里，靠 is_civilian_puppet == True 排除
//     (它 is_clone=False、is_puppet=False，这两个名字都骗人，别用)
//   - 机械玩偶 MyCivilianPuppetUnit 在 [2] 里、is_civilian_puppet=False，**保留**(用户要画它)
//   - owner_uid 别用：机械玩偶实测为 None，可能只在被操控时才有值
// ---- 模仿者模式(身份局，2026-09-24 两局实测，见 skill ruochen-teardown.md ⑦) ----
// 玩家是 units_by_type[1088] 的 CopycatPlayerUnit(12 人)，**不在键 1/2**。
//   identity_id    隐藏身份，进局约 2 秒后赋值、整局不变；全场真身份都下发给客户端
//   copycat_index  0~11，游戏里的号码 = 它 + 1
// 死人/隐身/灵魂(2026-11-10 反编译，见 skill copycat.md 四点三)：都读 combat 单位(unit.combat_unit)上的字段：
//   _is_dead            死人。单位还是 1088 那个，被切到鬼魂世界继续走 —— 不画、不进本体集合
//   _is_stealth         隐身(神偷 / 降灵师灵魂隐身)，活人，照画、名字后加「隐身」
//   InvokerComponent    降灵师(307)：is_soul = 灵魂形态(活人，照画、加「灵魂」)；is_changing2soul = 被投出后
//                       等下一轮变灵魂，这段在鬼魂世界，按死人处理
// [1089] GhostCopycatPlayerUnit 其实是**尸体**(和过场动画的克隆)，每轮开始/每次开会都被游戏清空，
// 炸弹/装箱/投票出局等死法还根本不生成 —— 以前拿它的 unique_id 认死者，漏得很多，已删。
static const int COPYCAT_PLAYER_TYPE = 1088;
static const int COPYCAT_INVOKER_ID = 307;        // 降灵师
// [1089] 尸体(2026-11-10 用户要画：灰框「N号 尸体」)。同一个键里还有过场动画的克隆(参数整份拷活人，没有 dead_reason)，
// 所以只认 dead_reason 是字符串的；被清洁工清理过的(combat 单位 has_cleand)不画。号码 = unique_id 对上的那个 1088 玩家
static const int COPYCAT_CORPSE_TYPE = 1089;
static const int COPYCAT_DIRECTOR_CLONE = 1210;   // CopycatDirectorCloneUnit = 导演局里的其他玩家
static const int COPYCAT_DIRECTOR_PUPPET = 1211;  // CopycatDirectorPuppetUnit = 导演傀儡(= 自身)
static const int COPYCAT_GHOUL_TYPE = 1203;       // CopycatGhoulUnit = 食人鬼(会杀人的地图生物,没身份)
static const int MAX_COPYCAT = 16;
static const int MAX_CARRIED = 8;   // 同时被扛在气球上的求生者上限(实际最多 4)
static const int MAX_HUNTERS = 4;   // 监管本体上限(联合狩猎 2 个)
// 走"号码+身份"身份簿的玩家类型键。导演局其他玩家是 1210(不在 1088、identity_id 直读为 None,要走
// combat_unit.components 遍历找身份组件,照若辰,见 read_identity);1211 是自身,靠 self 过滤掉。
static const int COPYCAT_PLAYER_TYPES[] = {COPYCAT_PLAYER_TYPE, COPYCAT_DIRECTOR_CLONE, COPYCAT_DIRECTOR_PUPPET};
static const int COPYCAT_PLAYER_TYPE_COUNT = sizeof(COPYCAT_PLAYER_TYPES) / sizeof(COPYCAT_PLAYER_TYPES[0]);
static inline bool is_copycat_player_type(int t) {
    return t == COPYCAT_PLAYER_TYPE || t == COPYCAT_DIRECTOR_CLONE || t == COPYCAT_DIRECTOR_PUPPET;
}
// identity 0 = 还没赋值/读不到；seat 从 1 开始，0 = 读不到；body 0 = 读不到
// flags_ok = 这一轮 dead/stealth/soul/changing/boxed/face_as 读成功了(读不到时身份簿沿用旧值)
// boxed   = 被送货员装着(GameStateComponent.game_state == Boxed)：位置每 tick 被设成送货员的，不画，改在送货员名字后列出
// face_as = 千面人变身中扮的号码(FaceManComponent.face_status == Change 时 tar_face_id + 1)，0 = 没变身
struct CopycatEntry { uint64_t body; int identity; int seat; bool flags_ok, dead, stealth, soul, changing, boxed; int face_as; };

struct Snapshot {
    uint64_t anchor;                  // 当前操控单位的场景对象，0 = 没读到
    int      camp;                  // 1=监管 2=求生者 其它=从属单位的 unit_type
    int64_t  uid;
    uint64_t stale[MAX_STALE];
    int      stale_count;
    uint64_t body[MAX_BODIES];      // 本体集合，见上
    bool     body_mirror[MAX_BODIES]; // 与 body[] 对齐：该本体是不是约瑟夫镜世界的镜像投影(out_photo_uid 有值)
    bool     body_in_photo[MAX_BODIES]; // 与 body[] 对齐：该(真实)本体当前在不在镜世界(它的 uid ∈ PhotoUnit._players)
    // 与 body[] 对齐：来自监管方的单位类型([1] 含巡视者、[236] 梦之信徒、[1065] 伊斯幻影)。
    // 只管骨骼颜色；butchers 只收 [1] 本体，信徒不能进那里(会污染预知和联合狩猎判定)
    bool     body_hunter[MAX_BODIES];
    int      body_count;
    // 约瑟夫镜世界：我当前操控的单位在不在镜世界里(anchor 的 in_photo_world 真 / out_photo_uid 有值)。
    // 配合 body_mirror 决定"另一个世界"：body_mirror != in_mirror 的本体画成白色鬼魂、不画天赋。
    bool     in_mirror;
    uint64_t hunter_body;              // [1] 里第一个类名不含 Puppet 的单位(跳过巡视者)，0 = 没有
    CopycatEntry copycat[MAX_COPYCAT]; // 模仿者模式的活人，见上
    int      copycat_count;
    // 被监管扛在气球上的求生者(b_catch==True)的 model+0x20 场景对象。监管视角用来过滤掉糊在
    // 自己身上的那个求生者框(2026-10-19)。实测 = 监管 carry_uid 指向的那个人，两者一致
    uint64_t carried[MAX_CARRIED];
    int      carried_count;
    // 自动盖板要判的目标(2026-10-24)：[1] 里全部监管本体(跳过巡视者/镜像，联合狩猎有两个)，
    // 梦之女巫换成她正在操控的那个信徒(见 collect_units)。hunter_body 仍只是第一个(预知用)
    uint64_t hunters[MAX_HUNTERS];
    bool     hunter_in_photo[MAX_HUNTERS]; // 与 hunters[] 对齐：在不在约瑟夫镜世界(uid ∈ PhotoUnit._players)
    bool     hunter_armor[MAX_HUNTERS];    // 与 hunters[] 对齐：super_armor(霸体，游戏 is_butcher_can_hit 直接不判)
    int      hunter_count;
    // 显示用的监管本体：[1] 里全部非巡视者非镜像(含女巫本体，不含信徒)。联合狩猎 2 个(2026-10-25)。
    // 固定信息/骨骼颜色/预知按它画；hunter_body 是其中第一个
    uint64_t butchers[MAX_HUNTERS];
    int      butcher_count;
    // butchers 里"我自己"的那个本体：= 锚点；我是女巫在操控信徒时锚点是信徒，靠她的操控 uid == 我的 uid 认出来。
    // 联合狩猎里我当监管，除了它以外的 butchers 就是队友(is_teammate_butcher)。0 = 不是监管/没认出来
    uint64_t self_butcher;
    int64_t  yidhra_ops[MAX_HUNTERS];      // 梦之女巫当前操控的单位 uid(_current_yidhra_puppet_uid)
    int      yidhra_op_count;
};

// 双缓冲发布：写线程填非活跃缓冲，填完再翻转，绘制线程永远读到完整的一份
static Snapshot g_buf[2];
static std::atomic<int> g_active{0};      // 见 PyProgress.h 同名变量的注释(volatile 不保证发布顺序)

static uintptr_t g_libbase = 0;
static uint64_t  g_module_dict = 0;
static int64_t   g_i_cam = -1, g_i_cam_unit = -1;
static int64_t   g_i_unit_mgr = -1, g_i_ubt = -1;
static int64_t   g_i_model = -1, g_i_another = -1, g_i_utype = -1, g_i_uid = -1;
// another_model 的序号缓存**按类分开存**(键 = 实例的 ob_type)。
// 各玩家类(ButcherUnit / CivilianUnit / MyCivilianUnit / 各种 Puppet ...)实例字典大小不同，
// another_model 的序号也不同；共用一个缓存时，遍历每换一个类就失效一次、从头按名字扫到它
// (字典上千条，每条 2 次读取)，一轮要扫 2~4 次，占了这个线程九成的读取量。
// 分开存之后每个类只在第一次出现时扫一次。缓存从不被直接信任：resolve_index() 每次都按名字核对，
// 核对不上就重扫，所以最坏情况就是退回共用缓存那种多扫几遍，不会读错属性。
// 表满了(类比这个多)就退回共用的 g_i_*。model / is_civilian_puppet 同理，一起按类存。
struct ClassIndexCache { uint64_t type; int64_t i_another; int64_t i_model; int64_t i_cpuppet; int64_t i_catch; bool is_puppet; bool is_chxy; };
static const int MAX_CLASSES = 8;
static ClassIndexCache g_class_cache[MAX_CLASSES];
static int     g_class_cache_count = 0;
static int64_t g_i_unit_model = -1, g_i_cpuppet = -1;   // 表满时的共用缓存
static int64_t g_i_catch = -1;                          // b_catch(被扛气球)的共用序号缓存
static int64_t g_i_outphoto = -1;                       // out_photo_uid(约瑟夫镜世界镜像判据)的共用序号缓存
// ---- 约瑟夫镜世界激活信号(2026-10-22 坐实) ----
// PhotoUnit 开局就持久存在,拍没拍照看它 _time：没拍照=None、拍照后=game_time(数字)。
// _players=当前在相中世界里的玩家 uid；我的 uid 在里面=我进了镜世界。实现见下面 scan_photo_unit。
// 声明放这里(collect_units 要用 g_photo_active 门控)
static char              g_photo_diag[160] = "";   // 空 = 未扫描，见 photo_diag_text()
static int64_t           g_photo_scan_ms = 0;
static int64_t           g_pd_i_time = -1, g_pd_i_players = -1;
static int               g_photo_key = -1;              // PhotoUnit 的 units_by_type 键(找到后缓存)
static uint64_t          g_photo_um_cache = 0;          // um 变了(换局)作废缓存
static std::atomic<bool> g_photo_active{false};         // PhotoUnit._time 有值 = 已拍照(镜世界激活)
static std::atomic<bool> g_photo_present{false};        // 场上存在 PhotoUnit = 约瑟夫局
static int64_t           g_photo_players[16];           // 当前在镜世界里的玩家 uid
static int               g_photo_players_n = 0;
static int64_t           g_i_body_uid = -1;             // 收本体时读 unit.uid 的序号缓存(判它在不在镜世界)
static int64_t g_i_yidhra = -1, g_i_puppet_uid = -1;    // 监管的 _current_yidhra_puppet_uid / 信徒的 uid
static int64_t g_i_armor = -1, g_i_puppet_armor = -1;   // 监管 / 信徒的 super_armor(两个类字典不同，分开缓存)
// 模仿者模式的玩家类(CopycatPlayerUnit 等)共用一组序号缓存，resolve_index 每次按名字核对
static int64_t g_cc_i_model = -1, g_cc_i_another = -1, g_cc_i_ident = -1, g_cc_i_index = -1;
static int64_t g_cc_i_combat = -1;                      // unit.combat_unit 的序号(读身份组件、死人/隐身用)
// combat 单位上的 _is_dead / _is_stealth / components，降灵师组件上的 is_soul / is_changing2soul，
// GameStateComponent.game_state，FaceManComponent.face_status / tar_face_id
static int64_t g_cc_i_cdead = -1, g_cc_i_cstealth = -1, g_cc_i_ccomps = -1, g_cc_i_soul = -1, g_cc_i_chg = -1;
static int64_t g_cc_i_gstate = -1, g_cc_i_fstatus = -1, g_cc_i_ftar = -1;
// combat 单位 -> 组件 的缓存(按类名找，找到就记住)。种类：0=InvokerComponent 1=FaceManComponent 2=GameStateComponent
enum { COMP_INVOKER = 0, COMP_FACEMAN = 1, COMP_GAMESTATE = 2, COMP_DIRECTOR = 3, COMP_KINDS = 4 };
static const char *const COMP_CLASS[COMP_KINDS] = { "InvokerComponent", "FaceManComponent", "GameStateComponent", "DirectorComponent" };

// ---- 导演操控傀儡(2026-11-10，机制见 skill copycat.md 四点三第 12 小节) ----
// 开会进顺序发言时，服务器把每个活人的 [1210] 分身摆到他开会前的位置；导演可以"离开会议"操控 [1211] 傀儡
// (g_cam_ctrl.unit = 傀儡、相机切普通视角)去刀分身 = 刀真人。这时会议桌边的真人没用，要画的是分身。
// 分身/傀儡跟真人同号(copycat_index 照抄)，所以按号码存；平时藏在 HIDE_COL_POS=(0,-65536,0)，高度 > -300 = 已摆出来。
// 刀人目标 = 导演自己的 DirectorComponent._now_kill_identity_id(服务器推给导演客户端)，绘制侧给目标画射线
struct DirectorView {
    bool  clone_ok[MAX_COPYCAT]; float cx[MAX_COPYCAT], cz[MAX_COPYCAT], cy[MAX_COPYCAT];   // 下标 = 号码 - 1
    bool  puppet_ok; float px, pz, py;
    int   target_identity;                                                                  // 0 = 没有
};
static DirectorView g_dir[2];
static std::atomic<int> g_dir_active{0};
static DirectorView g_dir_work;
static std::atomic<bool> g_puppet_op{false};      // 我正在操控导演傀儡(g_cam_ctrl.unit 的 unit_type == 1211)
static int64_t g_cc_i_dtarget = -1;
static uint64_t g_any_cu = 0;                     // 本轮读到的任一 combat 单位(找舞台系统用 .core)
static int64_t  g_seat_first_ms[MAX_COPYCAT];     // 每个号码本局第一次出现的时刻(「新号码」日志用)

// ---- 指挥家舞台(CopycatStageSystem)：我(非模仿者阵营)站在舞台区里时，台上其他人的模型被挪到假位置 ----
// 骨骼来自模型会画歪，所以台上的人不画骨骼(2026-11-10 用户要求)。判法照游戏 check_unit_pos_in_stage：
// 水平距离 ≤ stage_radius 且高度差 ≤ stage_height。系统对象 = 任一 combat 单位 .core.system_instances['systems_copycat.CopycatStageSystem']
struct StageView { bool active; float x, z, y, radius, height; };   // active = 我在舞台区里且不是模仿者阵营(游戏这时才挪模型)
static StageView g_stage[2];
static std::atomic<int> g_stage_active{0};
static uint64_t g_stage_sys = 0, g_stage_um = 0;
static int64_t  g_st_i_core = -1, g_st_i_sysinst = -1, g_st_i_in = -1, g_st_i_wolf = -1, g_st_i_center = -1, g_st_i_rad = -1, g_st_i_h = -1;
struct CompCache { uint64_t cu, comp; };
static CompCache g_comp_cache[COMP_KINDS][16];
// ---- 会议阶段(2026-11-10)：g_cam_ctrl.cam_handler 的类名 == CamModeCopycatMeeting ----
// 游戏开会各阶段都 set_cam_mode(COPYCAT_MEETING_MODE)，自己的 is_mode_copycat_meeting() 就是比这个。
// 读不到时沿用上次的值；换局清零。⚠ CamModeCopycatStatic 不是会议(击杀/投票过场、监控摄像头、密道都用)
static int64_t g_i_cam_handler = -1;
static std::atomic<bool> g_meeting{false};
static char g_cam_mode_name[48] = "";                   // 上次读到的相机模式类名(只为变化时打日志)
// position 的候选属性，学若辰的回退链：position 当前不是活值时(假面舞会把模型换成石像、
// 有的字段会失活)依次往后试。只要有一个读出合法 Vector3 就用。
static const char *const POS_ATTRS[6] = {
    "position", "visual_position", "logic_position", "pos", "world_position", "puppet_position"
};
static int64_t g_cc_i_posattr[6] = {-1, -1, -1, -1, -1, -1};

// ---- 模仿者模式的坐标：按 unit.position 画，绕开场景对象(学若辰，2026-09-26) ----
// 假面舞会时每个人的模型换成石像(场景数组 12 -> 24 个人物对象)，身份簿按 model+0x20 配对就配不上了。
// 若辰根本不按模型配对：身份读 unit 上的 identity_id，位置读 unit.position(再依次试 visual_position/…)，
// 自己用矩阵投影，所以换不换模型都无所谓。我们照做，只读 position 这一个。
// position 对象布局在密码机上验过(+0x10/+0x14/+0x18 = X/高度/Y，顺序同场景坐标 +0xa0/+0xa4/+0xa8)，
// 玩家身上没单独验过，所以加一道校准：第一次读到的值必须跟该人场景对象的坐标对上，才记下这个对象类型，
// 之后只认这个类型。一直校准不上 = 布局不对，绘制侧自动退回按场景对象画(旧行为)。
// 这个坐标每轮单独发布，不依赖锚点(开会时锚点常读不到)；单次读失败沿用上次的值 POS_HOLD_MS。
struct CopycatPos { bool ok; float x, z, y; };            // x / 高度 / y
static CopycatPos g_pos[2][MAX_COPYCAT];                   // 下标 = 号码 - 1
static std::atomic<int> g_pos_active{0};
static CopycatPos g_pos_work[MAX_COPYCAT];
static int64_t    g_pos_ms[MAX_COPYCAT];                   // 最后一次读成功的时刻，0 = 从没读到
static std::atomic<uint64_t> g_pos_type{0};                // position 对象的类型，校准通过后才非 0
static const int64_t POS_HOLD_MS = 300;
// 校准时 position 与场景坐标的容差(场景单位，约 11.9 单位 = 1 米)。守墓人那次实测两者逐位相同
static const float POS_CALIB_TOL = 2.0f;

// ---- 模仿者模式里真正"没号码没身份"、只能按 position 画固定标签的额外单位 ----
// 现在只剩食人鬼(地图生物)。导演分身/傀儡(1210/1211)已改走上面的"号码+身份"身份簿
// (2026-10-18,见 collect_copycat / read_identity):它们是玩家、带 identity(组件里)和 copycat_index,
// 以前当"无身份"盖固定"导演"标签是错的。普通模仿者局 1203 键不存在,read_list 返回 false,什么都不收。
static const int MAX_EXTRA = 12;
struct ExtraUnit { float x, z, y; uint8_t label; };          // label 索引 EXTRA_LABELS
static const char *const EXTRA_LABELS[1] = { "食人鬼" };
static const struct { int type; uint8_t label; } EXTRA_TYPES[1] = {
    {COPYCAT_GHOUL_TYPE, 0},   // CopycatGhoulUnit 食人鬼(会杀人的地图生物,橙框)
};
static const int EXTRA_TYPE_COUNT = sizeof(EXTRA_TYPES) / sizeof(EXTRA_TYPES[0]);
static ExtraUnit g_extra[2][MAX_EXTRA];
static int       g_extra_n[2] = {0, 0};
static std::atomic<int> g_extra_active{0};
static ExtraUnit g_extra_work[MAX_EXTRA];
static int       g_extra_work_n = 0;

// 尸体(见 COPYCAT_CORPSE_TYPE)：每轮整份发布，seat 0 = 号码没对上
static const int MAX_CORPSE = 16;
struct CorpseUnit { float x, z, y; int seat; };
static CorpseUnit g_corpse[2][MAX_CORPSE];
static int        g_corpse_n[2] = {0, 0};
static std::atomic<int> g_corpse_active{0};
static int64_t g_cc_i_uq = -1;                                   // 1088 玩家的 unique_id(有尸体时才读)
static int64_t g_cp_i_uq = -1, g_cp_i_reason = -1, g_cp_i_combat = -1, g_cp_i_clean = -1;

static int64_t now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static volatile bool g_available = false;
// 局外(大厅/准备阶段)：units_by_type 完整读出来了、而且没有键 1/2。
// 准备阶段 cam.unit 是 MyCityUnit，它的 model+0x20 读不出场景指针(2026-09-22 实测)，锚点那步会失败，
// 所以这个标志不挂在 快照/g_available 上，在锚点之前单独算、单独发布。读不全一律算"不是局外"
static volatile bool g_in_lobby = false;
static volatile int  g_fail_streak = 0;
static char g_status[128] = "";   // 空 = 未启动，见 status_text()

// ---- 公共 CPython 工具：实现在 PyCore.h，三个模块共用一份(见那里的文件头) ----
using PyCore::is_obj; using PyCore::str_equals; using PyCore::Dict; using PyCore::read_dict;
using PyCore::key_slot; using PyCore::value_slot; using PyCore::find_key; using PyCore::get_attr;
using PyCore::resolve_index; using PyCore::get_type_list; using PyCore::get_inst_dict;
using PyCore::read_int; using PyCore::read_list; using PyCore::get_scene_obj;

static bool resolve_module()
{
    const char *why = "";
    g_module_dict = PyCore::game_kernel_dict(why);
    if (!g_module_dict) { snprintf(g_status, sizeof(g_status), "%s", why); return false; }
    return true;
}

// 判断是不是局外。units_by_type 的每个键都要读成功才下结论 —— get_type_list() 返回 0 分不清
// "没有这个键"和"键对象所在页读不到"，拿它判会在对局中途的读失败里误判成局外、把人全藏掉。
// 局外 = 没有键 1(监管) 也没有键 2(求生者)。2026-09-22 准备阶段实测键只有 53/59/74/100/1009。
// 模仿者模式的玩家在键 1088，不算的话这个模式会被判成局外、人物整类不画
// 顺带给出 has_copycat(有没有模仿者玩家键)：没有就不用每轮跑 collect_copycat 那五次键扫描。
// 读不全返回 false 时 has_copycat 不动(调用方默认 true，照旧全扫)
static bool check_lobby(uint64_t ubt, bool &in_lobby, bool &has_copycat)
{
    Dict k;
    if (!read_dict(ubt, k) || k.n_entries <= 0) return false;
    bool has_player = false, cc = false;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (kp == 0) continue;                         // 已删除的条目
        if (!is_obj(kp) || getPtr64(kp + 8) != PyRoot::g_int_type) return false;
        int64_t sz = 0; uint32_t dg = 0;
        if (!vm_readv(kp + 16, &sz, 8) || !vm_readv(kp + 24, &dg, 4)) return false;
        if (sz == 1 && (dg == 1 || dg == 2 || is_copycat_player_type(dg))) has_player = true;
        if (sz == 1 && is_copycat_player_type(dg)) cc = true;
    }
    in_lobby = !has_player;
    has_copycat = cc;
    return true;
}

// 类名(PyTypeObject.tp_name，+0x18 是 char*)里含不含某个词
static bool class_name_has(uint64_t type, const char *word)
{
    uint64_t np = getPtr64(type + 0x18);
    if (!is_obj(np)) return false;
    char buf[48] = {0};                                // 最长的 MyButcherPatrolPuppetUnit 也才 25 字节
    if (!vm_readv(np, buf, sizeof(buf) - 1)) return false;
    return strstr(buf, word) != nullptr;
}
// 实测 [1] 里的巡视者是 MyButcherPatrolPuppetUnit，挑"监管本体"时要跳过它(厂长傀儡 ButcherPuppetUnit 同样带 Puppet)
static bool class_name_has_puppet(uint64_t type) { return class_name_has(type, "Puppet"); }
// 厂长(Leo)的怒火分身：LSkillLeoChxyCreate -> LSkillBase.create_CHXY 建的 ButcherChxyUnit(圣诞活动版 ChristmasChxyUnit)，
// 用监管完整模型、AI 追人。按单位类型它在 [105] 不该进 [1]，但实测被画成红色监管(= 进了本体集合)，
// 所以按类名兜底：照画，只是不当自动盖板目标(用户 2026-10-30 定的)
static bool class_name_has_chxy(uint64_t type) { return class_name_has(type, "Chxy"); }

static inline void add_unique(uint64_t *tbl, int &cnt, int cap, uint64_t sp)
{
    for (int j = 0; j < cnt; j++) if (tbl[j] == sp) return;
    if (cnt < cap) tbl[cnt++] = sp;
}

// 约瑟夫镜世界("photo world")鬼魂判据。2026-10-22 约瑟夫局 probe2 实测(cmd_joseph.py)：
// 约瑟夫局里 units_by_type 的每个真身都有一个镜像双胞胎(密码机 7->14、求生者 4->8)，成对存在整局：
//   真身   out_photo_uid = None、in_photo_uid = 镜像uid
//   镜像   out_photo_uid = 真身uid、in_photo_uid = None   <- 这个就是要过滤掉的"鬼魂"
// 所以判据 = out_photo_uid 读出一个 >0 的 uid。约瑟夫本人(photographer)两个都是 None，不受影响；
// 非约瑟夫局这俩属性恒 None(或不存在)，is_photo_ghost 恒 false，零副作用。
static inline bool is_photo_ghost(uint64_t d, const Dict &dk)
{
    g_i_outphoto = resolve_index(d, dk, g_i_outphoto, "out_photo_uid");
    if (g_i_outphoto < 0) return false;
    int64_t v = 0;
    return PyCore::read_int(getPtr64(value_slot(dk, g_i_outphoto)), v) && v > 0;
}

// 加本体 + 同步记录它是不是镜像(约瑟夫镜世界)、是不是监管方。新加入时才写，去重命中不覆盖
static inline void add_body(Snapshot &res, uint64_t sp, bool mirror, bool in_photo, bool hunter_side)
{
    for (int j = 0; j < res.body_count; j++) if (res.body[j] == sp) return;
    if (res.body_count < MAX_BODIES) {
        res.body_mirror[res.body_count] = mirror;
        res.body_in_photo[res.body_count] = in_photo;
        res.body_hunter[res.body_count] = hunter_side;
        res.body[res.body_count++] = sp;
    }
}

static inline void add_hunter(Snapshot &res, uint64_t sp, bool in_photo, bool armor)
{
    for (int j = 0; j < res.hunter_count; j++) if (res.hunters[j] == sp) return;
    if (res.hunter_count < MAX_HUNTERS) {
        res.hunter_in_photo[res.hunter_count] = in_photo;
        res.hunter_armor[res.hunter_count] = armor;
        res.hunters[res.hunter_count++] = sp;
    }
}

// super_armor == True。客户端 iCommonPlayer.set_super_armor / broken_wheel_super_armor 直接写这个属性：
// 兴奋、破轮轮形态、台球手球形态(BilliardComponent.billiard_change_model)都会置 True。属性不在/读不到按 False
static inline bool read_armor(uint64_t d, const Dict &dk, int64_t &idx)
{
    idx = resolve_index(d, dk, idx, "super_armor");
    return idx >= 0 && getPtr64(value_slot(dk, idx)) == PyRoot::g_true;
}

// 扫一类单位：
//   another_model -> 废弃模型黑名单
//   model         -> 本体集合(幻灯师分身 is_civilian_puppet==True 不收)
//   [1] 里第一个类名不含 Puppet 的 -> 监管本体(预知监管用)
//   [1] 全部监管 + 被操控的梦之信徒 -> 自动盖板目标(hunters)
static void collect_units(uint64_t ubt, int type, Snapshot &res)
{
    uint64_t lst = get_type_list(ubt, type);
    int64_t n = 0; uint64_t items = 0;
    if (!read_list(lst, n, items) || n == 0) return;

    for (int64_t i = 0; i < n; i++) {
        uint64_t inst = getPtr64(items + i * 8);
        uint64_t d = get_inst_dict(inst);
        if (!d) continue;
        Dict dk;
        if (!read_dict(d, dk)) continue;
        const bool is_mirror = is_photo_ghost(d, dk);   // 约瑟夫镜世界：该单位是不是镜像(out_photo_uid 有值)

        // 按实例的类取序号缓存(见 g_class_cache)。类型指针读不到、或表满了就用共用缓存
        // 别叫 type：会遮住参数 type(单位类型键)，导致下面 type == 1 永远不成立、hunter_body 永远拿不到
        uint64_t cls = getPtr64(inst + 8);
        ClassIndexCache *slot = nullptr;
        if (is_obj(cls)) {
            for (int j = 0; j < g_class_cache_count; j++)
                if (g_class_cache[j].type == cls) { slot = &g_class_cache[j]; break; }
            if (!slot && g_class_cache_count < MAX_CLASSES) {
                slot = &g_class_cache[g_class_cache_count++];
                slot->type = cls;
                slot->i_another = slot->i_model = slot->i_cpuppet = slot->i_catch = -1;
                slot->is_puppet = class_name_has_puppet(cls);    // 类名一个类只读一次
                slot->is_chxy = class_name_has_chxy(cls);
            }
        }
        const bool is_chxy = slot ? slot->is_chxy : class_name_has_chxy(cls);
        int64_t &i_another = slot ? slot->i_another : g_i_another;
        int64_t &i_model = slot ? slot->i_model : g_i_unit_model;
        int64_t &i_cpuppet = slot ? slot->i_cpuppet : g_i_cpuppet;

        // 实测所有玩家类都有 another_model(没有第二形态时值是 None)，查不到就跳过(不是错误)
        i_another = resolve_index(d, dk, i_another, "another_model");
        if (i_another >= 0) {
            uint64_t sp = get_scene_obj(getPtr64(value_slot(dk, i_another)));
            if (sp) add_unique(res.stale, res.stale_count, MAX_STALE, sp);
        }

        i_model = resolve_index(d, dk, i_model, "model");
        if (i_model < 0) continue;
        uint64_t body = get_scene_obj(getPtr64(value_slot(dk, i_model)));
        if (!body) continue;

        // 幻灯师分身：is_civilian_puppet 是 True 单例。属性不存在(监管类)或读不到都按"不是分身"
        i_cpuppet = resolve_index(d, dk, i_cpuppet, "is_civilian_puppet");
        if (i_cpuppet >= 0 && getPtr64(value_slot(dk, i_cpuppet)) == PyRoot::g_true) continue;

        // 这个(真实)单位当前在不在镜世界：它的 uid 在 PhotoUnit._players 里。投影不需要(白框无后缀)
        bool unit_in_photo = false;
        if (!is_mirror && g_photo_players_n > 0) {
            g_i_body_uid = resolve_index(d, dk, g_i_body_uid, "uid");
            int64_t uid = 0;
            if (g_i_body_uid >= 0 && read_int(getPtr64(value_slot(dk, g_i_body_uid)), uid))
                for (int p = 0; p < g_photo_players_n; p++) if (g_photo_players[p] == uid) { unit_in_photo = true; break; }
        }
        // 约瑟夫镜世界门控：没拍照(photo 未激活)时镜像投影(out_photo_uid>0)整体不收 →
        // 不在本体集合里 → 绘制侧方框/名字/距离/骨骼自然全不画。拍照后才收，按 body_mirror 画成白色。
        if (!(is_mirror && !g_photo_active.load())) add_body(res, body, is_mirror, unit_in_photo, type != 2);
        // 监管本体(预知监管)只认真身约瑟夫，镜像监管(若有)不当本体
        if (type == 1 && !is_mirror && !(slot ? slot->is_puppet : class_name_has_puppet(cls))) {
            if (res.hunter_body == 0) res.hunter_body = body;
            add_unique(res.butchers, res.butcher_count, MAX_HUNTERS, body);
            if (body == res.anchor) res.self_butcher = body;
            // 梦之女巫：_current_yidhra_puppet_uid 有值(操控自己时 = 自己的 uid)。游戏 is_butcher_can_hit 里
            // not bt.is_yidhra() 让本体永远砸不中，所以本体不进盖板目标，只记下她正在操控谁
            g_i_yidhra = resolve_index(d, dk, g_i_yidhra, "_current_yidhra_puppet_uid");
            int64_t op = 0;
            if (g_i_yidhra >= 0 && read_int(getPtr64(value_slot(dk, g_i_yidhra)), op)) {
                if (res.yidhra_op_count < MAX_HUNTERS) res.yidhra_ops[res.yidhra_op_count++] = op;
                if (res.uid != 0 && op == res.uid) res.self_butcher = body;   // 我是这个女巫(正在操控我的锚点)
            } else if (!is_chxy) {                              // 怒火分身不当盖板目标，见 class_name_has_chxy
                add_hunter(res, body, unit_in_photo, read_armor(d, dk, g_i_armor));
            }
        }
        // 梦之信徒：活着的游戏都能砸，但只收正在被操控的那个(追人的就是它)。[236] 在 [1] 之后扫，op 已就绪
        if (type == 236 && !is_mirror && !is_chxy && res.yidhra_op_count > 0) {
            g_i_puppet_uid = resolve_index(d, dk, g_i_puppet_uid, "uid");
            int64_t uid = 0;
            if (g_i_puppet_uid >= 0 && read_int(getPtr64(value_slot(dk, g_i_puppet_uid)), uid))
                for (int p = 0; p < res.yidhra_op_count; p++)
                    if (res.yidhra_ops[p] == uid) { add_hunter(res, body, unit_in_photo, read_armor(d, dk, g_i_puppet_armor)); break; }
        }

        // 求生者被监管扛在气球上：b_catch==True(实测 = 监管 carry_uid 指向的那个人)。
        // 收它的场景对象，监管视角据此过滤。只有 CivilianUnit([2]) 有这个属性，别的类 resolve 返回 -1
        if (type == 2) {
            int64_t &i_catch = slot ? slot->i_catch : g_i_catch;
            i_catch = resolve_index(d, dk, i_catch, "b_catch");
            if (i_catch >= 0 && getPtr64(value_slot(dk, i_catch)) == PyRoot::g_true)
                add_unique(res.carried, res.carried_count, MAX_CARRIED, body);
        }
    }
}

// 读 unit.position 到 out(x/高度/y)。position 每帧被赋成新对象，照 read_number 的做法：
// 读指针 -> 读对象 -> 再读一次指针确认没变，refcount > 0，最多三次。类型校准见 g_pos_type 那段注释
static bool read_position(uint64_t d, const Dict &dk, uint64_t body, float out[3])
{
    // 若辰直接信任 position，不跟场景坐标做硬校准。我们照做：按候选链依次试，第一个读出
    // 合法 Vector3 的就用。只保留一道"便宜的类型一致性"闸(锁定第一次读到的 Vector3 类型，
    // 之后只认这个类型)防止偶然读到别的对象；首次锁定时若 body 可读，顺手跟场景坐标比一下
    // 只为日志提示布局对不对，不作为是否采用的条件(以前这里校准不过就整个不画 -> 假面舞会被滤)。
    for (int a = 0; a < 6; a++) {
        g_cc_i_posattr[a] = resolve_index(d, dk, g_cc_i_posattr[a], POS_ATTRS[a]);
        if (g_cc_i_posattr[a] < 0) continue;
        uint64_t slot = value_slot(dk, g_cc_i_posattr[a]);
        for (int t = 0; t < 3; t++) {
            uint64_t vp = getPtr64(slot);
            if (!is_obj(vp)) break;                            // 这个候选当前是 None/垃圾，换下一个
            uint8_t b[0x1c];
            if (!vm_readv(vp, b, sizeof(b))) continue;         // 读失败重试本候选
            if (getPtr64(slot) != vp) continue;                // 指针变了(竞态)重试
            int64_t rc = 0; memcpy(&rc, b, 8);
            uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
            if (rc <= 0 || !is_obj(tp)) continue;
            float v[3]; memcpy(v, b + 0x10, 12);
            bool sane = true;
            for (int k = 0; k < 3; k++) if (!std::isfinite(v[k]) || fabsf(v[k]) > 1e5f) sane = false;
            if (!sane || (v[0] == 0 && v[2] == 0)) continue;

            uint64_t known = g_pos_type.load();
            if (known == 0) {
                g_pos_type = tp;                               // 锁定类型
                if (body) {                                    // 只为日志：布局对不对得上场景坐标
                    uint64_t coor = getPtr64(body + 0x28);
                    float s[3];
                    if (is_obj(coor) && vm_readv(coor + 0xa0, s, 12)) {
                        bool m = fabsf(s[0]-v[0]) <= POS_CALIB_TOL && fabsf(s[1]-v[1]) <= POS_CALIB_TOL
                              && fabsf(s[2]-v[2]) <= POS_CALIB_TOL;
                        printf("[模仿者] position 锁定 attr=%s 类型=0x%llx 场景坐标%s (%.1f,%.1f,%.1f)\n",
                               POS_ATTRS[a], (unsigned long long)tp, m ? "一致" : "不一致·仍采用", v[0], v[1], v[2]);
                        fflush(stdout);
                    }
                }
            } else if (tp != known) {
                break;                                         // 这个候选是别的类型，换下一个
            }
            memcpy(out, v, 12);
            return true;
        }
    }
    return false;
}

// 每轮都发布一次坐标(包括不在模仿者局时，全部 ok=false)。单次读失败沿用上次的值 POS_HOLD_MS
static void pos_publish(int64_t now)
{
    int w = 1 - g_pos_active;
    for (int i = 0; i < MAX_COPYCAT; i++) {
        g_pos[w][i] = g_pos_work[i];
        g_pos[w][i].ok = g_pos_ms[i] != 0 && now - g_pos_ms[i] <= POS_HOLD_MS;
    }
    g_pos_active = w;
}

// 读一个玩家单位的隐藏身份(identity_id, 100~399)。
// 先直读 unit.identity_id(普通模仿者局 1088 就挂这儿,已验证);allow_component=true 且直读没有时,
// 走 combat_unit.components 遍历找带 identity_id 的身份组件 —— 照若辰。
//   若辰实读(若辰正式版(4).sh @0x3B60F4,2026-10-18 反汇编确认):getattr "components"(兜底 "_components")
//   -> 把整个集合枚举出来 -> 逐个组件找 identity_id,**不按定键 1007 索引**(整段汇编没有 0x3ef 立即数)。
//   导演局的 CopycatDirectorClone/PuppetUnit 直读 identity_id 为 None,只能走这条。
static int read_identity(uint64_t d, const Dict &dk, bool allow_component)
{
    int64_t v = 0;
    g_cc_i_ident = resolve_index(d, dk, g_cc_i_ident, "identity_id");
    if (g_cc_i_ident >= 0 && read_int(getPtr64(value_slot(dk, g_cc_i_ident)), v) && v > 100 && v < 400)
        return (int)v;
    if (!allow_component) return 0;

    g_cc_i_combat = resolve_index(d, dk, g_cc_i_combat, "combat_unit");
    if (g_cc_i_combat < 0) return 0;
    uint64_t cud = get_inst_dict(getPtr64(value_slot(dk, g_cc_i_combat)));
    if (!cud) return 0;
    Dict cuk;
    if (!read_dict(cud, cuk)) return 0;
    int64_t ci = find_key(cud, "components", -1);
    if (ci < 0) ci = find_key(cud, "_components", -1);          // 候选名,照若辰
    if (ci < 0) return 0;
    uint64_t comps = getPtr64(value_slot(cuk, ci));

    // components 活体里是 dict(int 键 1007 -> 身份组件),但若辰不定键、遍历找;我们照做:取第一个能读出合法 identity_id 的组件
    Dict compk;
    if (read_dict(comps, compk)) {
        for (int64_t i = 0; i < compk.n_entries; i++) {
            uint64_t cd2 = get_inst_dict(getPtr64(value_slot(compk, i)));
            if (!cd2) continue;
            Dict cdk;
            if (!read_dict(cd2, cdk)) continue;
            int64_t idx = find_key(cd2, "identity_id", -1);
            if (idx >= 0 && read_int(getPtr64(value_slot(cdk, idx)), v) && v > 100 && v < 400)
                return (int)v;
        }
        return 0;
    }
    int64_t ln = 0; uint64_t litems = 0;                        // 兜底:万一 components 是 list 不是 dict
    if (read_list(comps, ln, litems)) {
        for (int64_t i = 0; i < ln; i++) {
            uint64_t cd2 = get_inst_dict(getPtr64(litems + i * 8));
            if (!cd2) continue;
            Dict cdk;
            if (!read_dict(cd2, cdk)) continue;
            int64_t idx = find_key(cd2, "identity_id", -1);
            if (idx >= 0 && read_int(getPtr64(value_slot(cdk, idx)), v) && v > 100 && v < 400)
                return (int)v;
        }
    }
    return 0;
}

static bool class_is(uint64_t obj, const char *name)
{
    uint64_t tp = is_obj(obj) ? getPtr64(obj + 8) : 0;
    uint64_t np = is_obj(tp) ? getPtr64(tp + 0x18) : 0;      // PyTypeObject.tp_name
    char buf[40] = {0};
    return is_obj(np) && vm_readv(np, buf, sizeof(buf) - 1) && strcmp(buf, name) == 0;
}

// 在 combat_unit.components(dict) 里按类名找组件，按 combat 单位缓存(缓存命中时仍核对一次类名)
static uint64_t find_comp(const PyCore::InstView &cv, uint64_t cu, int kind)
{
    CompCache *tbl = g_comp_cache[kind];
    for (int i = 0; i < 16; i++)
        if (tbl[i].cu == cu) {
            if (class_is(tbl[i].comp, COMP_CLASS[kind])) return tbl[i].comp;
            tbl[i].cu = 0; break;
        }
    bool has = false;
    uint64_t comps = PyCore::inst_attr(cv, g_cc_i_ccomps, "components", has);
    Dict ck;
    if (!has || !read_dict(comps, ck)) return 0;
    for (int64_t i = 0; i < ck.n_entries; i++) {
        uint64_t comp = getPtr64(value_slot(ck, i));
        if (!class_is(comp, COMP_CLASS[kind])) continue;
        int slot = 0;                                             // 空位优先，满了覆盖第 0 个
        for (int j = 0; j < 16; j++) if (tbl[j].cu == 0) { slot = j; break; }
        tbl[slot] = {cu, comp};
        return comp;
    }
    return 0;
}

// 读组件上一个 int 属性
static bool comp_int(uint64_t comp, int64_t &idx, const char *name, int64_t &out)
{
    PyCore::InstView v;
    bool has = false;
    if (!PyCore::inst_view(comp, v)) return false;
    PyCore::inst_attr(v, idx, name, has);
    return has && read_int(getPtr64(PyCore::inst_value_slot(v, idx)), out);
}

// 读 combat 单位上的 死人/隐身/灵魂/被装/千面人变身。成功返回 nullptr，失败返回原因(日志用)，e 的这几项不动
static const char *read_combat_state(uint64_t d, const Dict &dk, int identity, CopycatEntry &e)
{
    g_cc_i_combat = resolve_index(d, dk, g_cc_i_combat, "combat_unit");
    if (g_cc_i_combat < 0) return "unit 上没有 combat_unit";
    uint64_t cu = getPtr64(value_slot(dk, g_cc_i_combat));
    PyCore::InstView cv;
    if (!PyCore::inst_view(cu, cv)) return "combat_unit 读不到";
    if (!g_any_cu) g_any_cu = cu;
    bool has = false, dead = false, stealth = false;
    PyCore::inst_attr(cv, g_cc_i_cdead, "_is_dead", has);
    if (!has || !PyCore::read_bool(PyCore::inst_value_slot(cv, g_cc_i_cdead), dead)) return "_is_dead 读不到";
    PyCore::inst_attr(cv, g_cc_i_cstealth, "_is_stealth", has);
    if (!has || !PyCore::read_bool(PyCore::inst_value_slot(cv, g_cc_i_cstealth), stealth)) return "_is_stealth 读不到";
    bool soul = false, changing = false;
    if (identity == COPYCAT_INVOKER_ID) {
        uint64_t comp = find_comp(cv, cu, COMP_INVOKER);
        PyCore::InstView iv;
        if (!comp || !PyCore::inst_view(comp, iv)) return "降灵师组件找不到";
        PyCore::inst_attr(iv, g_cc_i_soul, "is_soul", has);
        if (!has || !PyCore::read_bool(PyCore::inst_value_slot(iv, g_cc_i_soul), soul)) return "is_soul 读不到";
        PyCore::inst_attr(iv, g_cc_i_chg, "is_changing2soul", has);
        if (!has || !PyCore::read_bool(PyCore::inst_value_slot(iv, g_cc_i_chg), changing)) return "is_changing2soul 读不到";
    }
    // 被送货员装着：GameState.Boxed = 2(每个玩家都有这个组件)
    int64_t gs = 0;
    uint64_t gcomp = find_comp(cv, cu, COMP_GAMESTATE);
    if (!gcomp) return "GameStateComponent 找不到";
    if (!comp_int(gcomp, g_cc_i_gstate, "game_state", gs)) return "game_state 读不到";
    // 导演：本轮刀人目标身份(None 时 read_int 失败 = 0)。只有导演自己的客户端有值
    if (identity == 308) {
        uint64_t dcomp = find_comp(cv, cu, COMP_DIRECTOR);
        int64_t tid = 0;
        if (dcomp && comp_int(dcomp, g_cc_i_dtarget, "_now_kill_identity_id", tid) && tid > 100 && tid < 400)
            g_dir_work.target_identity = (int)tid;
    }
    // 千面人变身：FaceStatus.Change = 3，扮的是 tar_face_id(= 目标 copycat_index)
    int face_as = 0;
    if (identity == 202) {
        uint64_t fcomp = find_comp(cv, cu, COMP_FACEMAN);
        if (!fcomp) return "FaceManComponent 找不到";
        int64_t st = 0, tar = -1;
        if (!comp_int(fcomp, g_cc_i_fstatus, "face_status", st)) return "face_status 读不到";
        if (st == 3 && comp_int(fcomp, g_cc_i_ftar, "tar_face_id", tar) && tar >= 0 && tar < MAX_COPYCAT) face_as = (int)tar + 1;
    }
    e.flags_ok = true;
    e.dead = dead; e.stealth = stealth && !dead;
    e.soul = soul && !dead; e.changing = changing && !dead;
    e.boxed = gs == 2 && !dead;
    e.face_as = dead ? 0 : face_as;
    return nullptr;
}

// 读一个 math3d.vector(+0x10/+0x14/+0x18 = X/高度/Y)，类型必须是 position 校准时锁定的那个
static bool read_vec3(uint64_t vo, float out[3])
{
    uint64_t want = g_pos_type.load();
    if (!is_obj(vo) || !want) return false;
    uint8_t b[0x1c];
    if (!vm_readv(vo, b, sizeof(b))) return false;
    uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
    if (tp != want) return false;
    memcpy(out, b + 0x10, 12);
    for (int k = 0; k < 3; k++) if (!std::isfinite(out[k]) || fabsf(out[k]) > 1e5f) return false;
    return true;
}

static void stage_publish(const StageView &v)
{
    int w = 1 - g_stage_active.load();
    g_stage[w] = v;
    g_stage_active.store(w);
}

// 指挥家舞台状态(见 StageView)。系统对象每局找一次并缓存；找不到时 1 秒重试一次
static void scan_stage(uint64_t um)
{
    static int64_t last_try = 0;
    static int last_state = -1;                   // 日志：-1 未知 0 没找到系统 1 找到/不在台上 2 在台上
    StageView sv; memset(&sv, 0, sizeof(sv));
    if (um != g_stage_um) { g_stage_um = um; g_stage_sys = 0; }
    bool has = false;
    if (!g_stage_sys && g_any_cu && now_ms() - last_try >= 1000) {
        last_try = now_ms();
        PyCore::InstView cv, kv;
        if (PyCore::inst_view(g_any_cu, cv)) {
            uint64_t core = PyCore::inst_attr(cv, g_st_i_core, "core", has);
            if (has && PyCore::inst_view(core, kv)) {
                uint64_t si = PyCore::inst_attr(kv, g_st_i_sysinst, "system_instances", has);
                int64_t i = has ? find_key(si, "systems_copycat.CopycatStageSystem", -1) : -1;
                if (i >= 0) g_stage_sys = get_attr(si, i);
                static uint64_t dumped_um = 0;                 // 找不到时把键名打一次(每局一次)，核对键格式
                Dict sk;
                if (i < 0 && has && dumped_um != um && read_dict(si, sk)) {
                    dumped_um = um;
                    printf("[模仿者] system_instances 共%lld个键:", (long long)sk.n_entries);
                    for (int64_t j = 0; j < sk.n_entries && j < 80; j++) {
                        char kn[64];
                        if (PyCore::read_str(getPtr64(key_slot(sk, j)), kn, sizeof(kn))) printf(" %s", kn);
                    }
                    printf("\n");
                    fflush(stdout);
                }
            }
        }
    }
    PyCore::InstView v;
    if (g_stage_sys && !PyCore::inst_view(g_stage_sys, v)) g_stage_sys = 0;
    if (g_stage_sys) {
        bool in = false, wolf = false;
        PyCore::inst_attr(v, g_st_i_in, "_g_unit_in_area", has);
        if (has) PyCore::read_bool(PyCore::inst_value_slot(v, g_st_i_in), in);
        PyCore::inst_attr(v, g_st_i_wolf, "gunit_is_wolf", has);
        if (has) PyCore::read_bool(PyCore::inst_value_slot(v, g_st_i_wolf), wolf);
        float c[3], rad = 0, h = 0;
        uint64_t cp = PyCore::inst_attr(v, g_st_i_center, "stage_center_pos", has);
        bool cok = has && read_vec3(cp, c);
        PyCore::inst_attr(v, g_st_i_rad, "stage_radius", has);
        bool rok = has && PyCore::read_number(PyCore::inst_value_slot(v, g_st_i_rad), rad, 0.0, 1e5);
        PyCore::inst_attr(v, g_st_i_h, "stage_height", has);
        bool hok = has && PyCore::read_number(PyCore::inst_value_slot(v, g_st_i_h), h, 0.0, 1e5);
        if (in && !wolf && cok && rok && hok && rad > 0) {
            sv.active = true; sv.x = c[0]; sv.z = c[1]; sv.y = c[2]; sv.radius = rad; sv.height = h;
        }
    }
    int st = !g_stage_sys ? 0 : (sv.active ? 2 : 1);
    if (st != last_state) {
        last_state = st;
        if (st == 2) printf("[模仿者] 舞台：我在台上 中心(%.1f,%.1f,%.1f) 半径%.1f 高%.1f，台上的人不画骨骼\n", sv.x, sv.z, sv.y, sv.radius, sv.height);
        else printf("[模仿者] 舞台：%s\n", st == 0 ? "没找到 CopycatStageSystem" : "系统已找到，我不在台上(或是模仿者阵营)");
        fflush(stdout);
    }
    stage_publish(sv);
}

// 模仿者模式：玩家单位(1088 普通 + 1210 导演局其他玩家 + 1211 导演傀儡=自身)的 model+0x20 进本体集合，
// 同时记下身份、号码(copycat_index)、unit.position 和 死人/隐身/灵魂(read_combat_state)。
// 导演局其他玩家是 CopycatDirectorCloneUnit(不在 1088、身份直读为 None):以前被当"无身份"盖固定"导演"标签,
// 现在跟 1088 同路 —— 身份走 combat_unit.components(照若辰),号码走 copycat_index,按"N号 身份"进身份簿。
// 1211 导演傀儡是自身,靠身份簿 self_seat / 绘制侧贴身<1m 过滤掉,不会误画。
static void collect_copycat(uint64_t ubt, uint64_t um_for_stage, Snapshot &res)
{
    const int64_t now = now_ms();
    static char last_err[48] = "(未读)";      // 状态读失败原因，变了才打日志(第一次读成功也会打一行)
    bool seat_seen[MAX_COPYCAT] = {false};    // 这一轮已经收过的号码(去重见玩家循环)
    memset(&g_dir_work, 0, sizeof(g_dir_work));
    g_any_cu = 0;

    // ---- 尸体：先收位置和 unique_id，号码在下面玩家循环里按 unique_id 对 ----
    struct CorpseWork { float x, z, y; char uq[24]; int seat; };
    CorpseWork cw[MAX_CORPSE];
    int cn = 0, c_total = 0, c_clone = 0, c_clean = 0;
    {
        int64_t gn = 0; uint64_t gitems = 0;
        if (read_list(get_type_list(ubt, COPYCAT_CORPSE_TYPE), gn, gitems)) {
            for (int64_t i = 0; i < gn && cn < MAX_CORPSE; i++) {
                uint64_t d = get_inst_dict(getPtr64(gitems + i * 8));
                Dict dk;
                if (!d || !read_dict(d, dk)) continue;
                c_total++;
                char rs[32];
                g_cp_i_reason = resolve_index(d, dk, g_cp_i_reason, "dead_reason");
                if (g_cp_i_reason < 0 || !PyCore::read_str(getPtr64(value_slot(dk, g_cp_i_reason)), rs, sizeof(rs))) { c_clone++; continue; }
                g_cp_i_combat = resolve_index(d, dk, g_cp_i_combat, "combat_unit");
                PyCore::InstView cv;
                if (g_cp_i_combat >= 0 && PyCore::inst_view(getPtr64(value_slot(dk, g_cp_i_combat)), cv)) {
                    bool has = false, clean = false;
                    PyCore::inst_attr(cv, g_cp_i_clean, "has_cleand", has);
                    if (has && PyCore::read_bool(PyCore::inst_value_slot(cv, g_cp_i_clean), clean) && clean) { c_clean++; continue; }
                }
                float p[3];
                if (!read_position(d, dk, 0, p)) continue;
                CorpseWork &w = cw[cn++];
                w.x = p[0]; w.z = p[1]; w.y = p[2]; w.seat = 0; w.uq[0] = 0;
                g_cp_i_uq = resolve_index(d, dk, g_cp_i_uq, "unique_id");
                if (g_cp_i_uq >= 0) PyCore::read_str(getPtr64(value_slot(dk, g_cp_i_uq)), w.uq, sizeof(w.uq));
            }
        }
        static int last_total = -1, last_n = -1, last_clean = -1;
        if (c_total != last_total || cn != last_n || c_clean != last_clean) {
            printf("[模仿者] 1089 共%d 尸体%d 过场克隆%d 已清理%d\n", c_total, cn, c_clone, c_clean);
            fflush(stdout);
            last_total = c_total; last_n = cn; last_clean = c_clean;
        }
    }

    for (int pt = 0; pt < COPYCAT_PLAYER_TYPE_COUNT; pt++) {
        const int ptype = COPYCAT_PLAYER_TYPES[pt];
        // 导演类直读 identity_id 为 None,才走组件遍历;1088 维持原直读(零回归、不触发组件遍历的开销)
        const bool allow_component = (ptype != COPYCAT_PLAYER_TYPE);
        int64_t n = 0; uint64_t items = 0;
        if (!read_list(get_type_list(ubt, ptype), n, items) || n == 0) continue;

        for (int64_t i = 0; i < n; i++) {
            uint64_t d = get_inst_dict(getPtr64(items + i * 8));
            Dict dk;
            if (!d || !read_dict(d, dk)) continue;

            g_cc_i_another = resolve_index(d, dk, g_cc_i_another, "another_model");
            if (g_cc_i_another >= 0) {
                uint64_t sp = get_scene_obj(getPtr64(value_slot(dk, g_cc_i_another)));
                if (sp) add_unique(res.stale, res.stale_count, MAX_STALE, sp);
            }

            // 每一项都可能单独读失败(内存压力下页被换出)，读不到的留 0，交给身份簿沿用旧值
            uint64_t body = 0;
            g_cc_i_model = resolve_index(d, dk, g_cc_i_model, "model");
            if (g_cc_i_model >= 0) body = get_scene_obj(getPtr64(value_slot(dk, g_cc_i_model)));

            CopycatEntry e;
            memset(&e, 0, sizeof(e));
            e.body = body;
            int64_t v = 0;
            g_cc_i_index = resolve_index(d, dk, g_cc_i_index, "copycat_index");
            if (g_cc_i_index >= 0 && read_int(getPtr64(value_slot(dk, g_cc_i_index)), v) && v >= 0 && v < MAX_COPYCAT)
                e.seat = (int)v + 1;
            // 同一号码一轮只认第一份(2026-11-10 导演局实测)：导演局里 [1210] 导演分身和 [1088] 真人带同一个 copycat_index，
            // 两份都写身份簿同一格时，状态每轮被分身覆盖(降灵师「等变灵魂」1/0 来回跳了 1.1 万行)、坐标也被换成分身的。
            // [1088] 在 COPYCAT_PLAYER_TYPES 最前面，所以真人优先；重复的只照旧进本体集合，不读状态/坐标、不进簿子
            if (e.seat > 0) {
                if (seat_seen[e.seat - 1]) {
                    if (body) add_unique(res.body, res.body_count, MAX_BODIES, body);
                    // 导演分身/傀儡：记坐标(导演操控傀儡时画分身、按傀儡算距离)
                    float p[3];
                    if ((ptype == COPYCAT_DIRECTOR_CLONE || ptype == COPYCAT_DIRECTOR_PUPPET) && read_position(d, dk, 0, p) && p[1] > -300.0f) {
                        if (ptype == COPYCAT_DIRECTOR_CLONE) {
                            int k = e.seat - 1;
                            g_dir_work.clone_ok[k] = true; g_dir_work.cx[k] = p[0]; g_dir_work.cz[k] = p[1]; g_dir_work.cy[k] = p[2];
                        } else {
                            g_dir_work.puppet_ok = true; g_dir_work.px = p[0]; g_dir_work.pz = p[1]; g_dir_work.py = p[2];
                        }
                    }
                    continue;
                }
                seat_seen[e.seat - 1] = true;
            }
            e.identity = read_identity(d, dk, allow_component);
            // 每局每个号码第一次出现时打一行：来自哪个键、单位类名、身份、坐标(查"会议上多出一个 13号 ?"这类来历不明的号码)
            {
                static uint64_t seen_um = 0;
                static bool logged[MAX_COPYCAT];
                if (um_for_stage != seen_um) { seen_um = um_for_stage; memset(logged, 0, sizeof(logged)); memset(g_seat_first_ms, 0, sizeof(g_seat_first_ms)); }
                const int64_t first = e.seat > 0 ? g_seat_first_ms[e.seat - 1] : 0;
                if (e.seat > 0 && !logged[e.seat - 1] && (e.identity || (first && now - first > 5000))) {
                    logged[e.seat - 1] = true;
                    uint64_t inst = getPtr64(items + i * 8);
                    uint64_t tp = is_obj(inst) ? getPtr64(inst + 8) : 0;
                    uint64_t np = is_obj(tp) ? getPtr64(tp + 0x18) : 0;
                    char cls[40] = "?";
                    if (is_obj(np)) vm_readv(np, cls, sizeof(cls) - 1);
                    float p[3] = {0, 0, 0};
                    read_position(d, dk, 0, p);
                    printf("[模仿者] 新号码 %d号 键=%d 类=%s 身份%d 坐标(%.1f,%.1f,%.1f)\n", e.seat, ptype, cls, e.identity, p[0], p[1], p[2]);
                    fflush(stdout);
                } else if (e.seat > 0 && !g_seat_first_ms[e.seat - 1]) {
                    g_seat_first_ms[e.seat - 1] = now;               // 身份还没赋值：最多等 5 秒再打(真玩家约 2 秒就有身份)
                }
            }
            const char *err = read_combat_state(d, dk, e.identity, e);
            if (strcmp(err ? err : "", last_err) != 0) {
                snprintf(last_err, sizeof(last_err), "%s", err ? err : "");
                printf("[模仿者] 死人/隐身状态读取%s%s\n", err ? "失败: " : "正常", err ? err : "");
                fflush(stdout);
            }
            // 死人和"被投出、等变灵魂"的降灵师都在鬼魂世界；被送货员装着的人位置贴在送货员身上 ——
            // 这几种都不进本体集合、不读坐标、不画(状态这轮读不到时照旧收；身份簿那边沿用旧值，按簿子画的不受影响)
            const bool is_dead = e.dead || e.changing || e.boxed;
            if (body && !is_dead) add_unique(res.body, res.body_count, MAX_BODIES, body);
            if (res.copycat_count >= MAX_COPYCAT) continue;
            res.copycat[res.copycat_count++] = e;
            if (cn > 0 && e.seat > 0) {                 // 尸体号码：unique_id 跟这个玩家对上
                char uq[24];
                g_cc_i_uq = resolve_index(d, dk, g_cc_i_uq, "unique_id");
                if (g_cc_i_uq >= 0 && PyCore::read_str(getPtr64(value_slot(dk, g_cc_i_uq)), uq, sizeof(uq)) && uq[0])
                    for (int j = 0; j < cn; j++) if (cw[j].seat == 0 && strcmp(cw[j].uq, uq) == 0) cw[j].seat = e.seat;
            }

            // 坐标只给活人读；号码读不到就不知道记到哪一格
            float p[3];
            if (e.seat > 0 && !is_dead && read_position(d, dk, body, p)) {
                CopycatPos &w = g_pos_work[e.seat - 1];
                w.x = p[0]; w.z = p[1]; w.y = p[2];
                g_pos_ms[e.seat - 1] = now;
            }
        }
    }

    {
        int cwi = 1 - g_corpse_active.load();
        for (int j = 0; j < cn; j++) g_corpse[cwi][j] = {cw[j].x, cw[j].z, cw[j].y, cw[j].seat};
        g_corpse_n[cwi] = cn;
        g_corpse_active.store(cwi);
    }
    {
        int dw = 1 - g_dir_active.load();
        g_dir[dw] = g_dir_work;
        g_dir_active.store(dw);
    }
    scan_stage(um_for_stage);

    // 额外单位:真正没号码没身份、只能按 position 画固定标签的 —— 只剩食人鬼(地图生物)。
    // 导演分身/傀儡(1210/1211)已改走上面的"号码+身份"身份簿。普通模仿者局 1203 键不存在,收不到东西。
    g_extra_work_n = 0;
    for (int ti = 0; ti < EXTRA_TYPE_COUNT; ti++) {
        int64_t en = 0; uint64_t eitems = 0;
        if (!read_list(get_type_list(ubt, EXTRA_TYPES[ti].type), en, eitems)) continue;
        for (int64_t i = 0; i < en && g_extra_work_n < MAX_EXTRA; i++) {
            uint64_t de = get_inst_dict(getPtr64(eitems + i * 8));
            Dict dke;
            if (!de || !read_dict(de, dke)) continue;
            float p[3];
            if (!read_position(de, dke, 0, p)) continue;    // body=0：这些单位本就不做模型配对
            ExtraUnit &x = g_extra_work[g_extra_work_n++];
            x.x = p[0]; x.z = p[1]; x.y = p[2]; x.label = EXTRA_TYPES[ti].label;
        }
    }
    int ew = 1 - g_extra_active.load();
    memcpy(g_extra[ew], g_extra_work, sizeof(ExtraUnit) * g_extra_work_n);
    g_extra_n[ew] = g_extra_work_n;
    g_extra_active.store(ew);
}

// ---- 身份簿(锁存)：模仿者开会阶段游戏负载低，Python 堆页会被换进 zram，身份/模型全读不到 ----
// 思路同 PyGenius.h 的天赋锁存，但简单得多：身份整局不变，按号码(copycat_index)记，
//   - 读到就写进簿子(新读到的值永远覆盖旧值 —— 所以万一没清干净，新一局第一次读到就自动纠正)
//   - 读不到就沿用簿子
//   - 死亡只会置上、不会撤销；隐身/灵魂/等变灵魂每轮跟着读到的值走(读不到沿用)
//   - unit_mgr 地址变了(换局/回大厅)就整本清空
// 只在界面上「模仿者模式」打开时记(g_book_enabled)，关掉就清空。
// 发布方式跟 Snapshot 一样双缓冲，但**不依赖锚点成功** —— 锚点那步失败时簿子照样更新和发布
// 状态变化打日志 [模仿者] N号 ...(上机核对 _is_dead 等字段名用，pull hack_stdout.log 看)
struct CopycatSeat { bool known; uint64_t body; int identity; bool dead, stealth, soul, changing, boxed; int face_as; };
struct CopycatBook { CopycatSeat seat[MAX_COPYCAT]; int self_seat; };   // seat[号码-1]；self_seat 从 1 开始，0 = 不知道
static CopycatBook g_book[2];
static std::atomic<int> g_book_active{0};
static CopycatBook g_book_work;                 // 只有 PySelf 线程碰
static uint64_t g_book_um = 0;
static std::atomic<bool> g_book_enabled{false};

static void book_publish()
{
    int w = 1 - g_book_active;
    g_book[w] = g_book_work;
    g_book_active = w;
}

static bool book_empty(const CopycatBook &b)
{
    for (int i = 0; i < MAX_COPYCAT; i++) if (b.seat[i].known) return false;
    return true;
}

static void book_merge(const Snapshot &res, uint64_t um)
{
    if (!g_book_enabled) {
        if (!book_empty(g_book_work)) { memset(&g_book_work, 0, sizeof(g_book_work)); book_publish(); }
        g_book_um = 0;
        return;
    }
    bool changed = false;
    if (is_obj(um) && um != g_book_um) {         // um 读不到(0)时什么都不动
        if (!book_empty(g_book_work)) changed = true;
        memset(&g_book_work, 0, sizeof(g_book_work));
        g_book_um = um;
    }
    for (int i = 0; i < res.copycat_count; i++) {
        const CopycatEntry &e = res.copycat[i];
        if (e.seat < 1 || e.seat > MAX_COPYCAT) continue;       // 号码读不到就不知道记到哪一格
        CopycatSeat &b = g_book_work.seat[e.seat - 1];
        if (!b.known) { b.known = true; changed = true; }
        if (e.body && e.body != b.body) { b.body = e.body; changed = true; }
        if (e.identity && e.identity != b.identity) { b.identity = e.identity; changed = true; }
        if (!e.flags_ok) continue;
        bool dead = b.dead || e.dead;
        if (dead != b.dead || e.stealth != b.stealth || e.soul != b.soul || e.changing != b.changing
            || e.boxed != b.boxed || e.face_as != b.face_as) {
            b.dead = dead; b.stealth = e.stealth; b.soul = e.soul; b.changing = e.changing;
            b.boxed = e.boxed; b.face_as = e.face_as;
            changed = true;
            printf("[模仿者] %d号 身份%d 死=%d 隐身=%d 灵魂=%d 等变灵魂=%d 被装=%d 扮=%d\n",
                   e.seat, b.identity, (int)b.dead, (int)b.stealth, (int)b.soul, (int)b.changing, (int)b.boxed, b.face_as);
            fflush(stdout);
        }
    }
    if (changed) book_publish();
}

static void book_mark_self(uint64_t anchor)
{
    if (!g_book_enabled || anchor == 0) return;
    for (int i = 0; i < MAX_COPYCAT; i++) {
        if (g_book_work.seat[i].known && g_book_work.seat[i].body == anchor) {
            if (g_book_work.self_seat != i + 1) { g_book_work.self_seat = i + 1; book_publish(); }
            return;
        }
    }
}

// ---- 约瑟夫镜世界激活信号 + in_mirror(2026-10-22 坐实，取代旧的读 anchor in_photo_world) ----
// 走叠加层自己的内核驱动读 Python 对象图(同 PySelf，是允许通道、不碰 /proc；probe2 死了不影响)。
// PhotoUnit 一局内稳定 → 键找到后缓存，之后每轮只读这一个单位；没找到(非约瑟夫局)全扫节流 500ms。
static inline bool is_photo_class(uint64_t inst)
{
    uint64_t cls = getPtr64(inst + 8);
    if (!is_obj(cls)) return false;
    uint64_t np = getPtr64(cls + 0x18);                          // PyTypeObject.tp_name
    char buf[48] = {0};
    return is_obj(np) && vm_readv(np, buf, sizeof(buf) - 1) && strstr(buf, "Photo");
}

static uint64_t find_photo_inst(uint64_t ubt)
{
    Dict k;
    if (!read_dict(ubt, k)) return 0;
    for (int64_t i = 0; i < k.n_entries; i++) {
        uint64_t kp = getPtr64(key_slot(k, i));
        if (!is_obj(kp) || getPtr64(kp + 8) != PyRoot::g_int_type) continue;
        int64_t sz = 0; uint32_t dg = 0;
        if (!vm_readv(kp + 16, &sz, 8) || !vm_readv(kp + 24, &dg, 4) || sz != 1) continue;
        int64_t n = 0; uint64_t items = 0;
        if (!read_list(getPtr64(value_slot(k, i)), n, items) || n == 0) continue;
        for (int64_t j = 0; j < n; j++) {
            uint64_t inst = getPtr64(items + j * 8);
            if (is_photo_class(inst)) { g_photo_key = (int)dg; return inst; }
        }
    }
    return 0;
}

static void scan_photo_unit(uint64_t ubt, uint64_t um)
{
    if (um != g_photo_um_cache) { g_photo_um_cache = um; g_photo_key = -1; }   // 换局作废
    uint64_t inst = 0;
    if (g_photo_key >= 0) {                                       // 缓存命中：只读这一个单位
        int64_t n = 0; uint64_t items = 0;
        if (read_list(get_type_list(ubt, g_photo_key), n, items) && n > 0 && is_photo_class(getPtr64(items)))
            inst = getPtr64(items);
        else g_photo_key = -1;                                    // 失效 → 下面全扫
    }
    if (!inst && now_ms() - g_photo_scan_ms >= 500) { g_photo_scan_ms = now_ms(); inst = find_photo_inst(ubt); }

    bool active = false; g_photo_players_n = 0;
    char tbuf[24] = "?"; int players = -1;
    if (inst) {
        uint64_t d = get_inst_dict(inst);
        Dict dk;
        if (d && read_dict(d, dk)) {
            g_pd_i_time = resolve_index(d, dk, g_pd_i_time, "_time");
            if (g_pd_i_time >= 0) {                               // _time 是数字=已拍照，None=没拍照
                float t = 0;
                if (PyCore::read_number(value_slot(dk, g_pd_i_time), t, -1e12, 1e12)) { active = true; snprintf(tbuf, sizeof(tbuf), "%.0f", t); }
                else snprintf(tbuf, sizeof(tbuf), "none");
            }
            g_pd_i_players = resolve_index(d, dk, g_pd_i_players, "_players");
            if (g_pd_i_players >= 0) {                            // _players = 当前在镜世界的玩家 uid
                int64_t pn = 0; uint64_t pit = 0;
                if (read_list(getPtr64(value_slot(dk, g_pd_i_players)), pn, pit)) {
                    players = (int)pn;
                    for (int64_t j = 0; j < pn && g_photo_players_n < 16; j++) {
                        int64_t uid = 0;
                        if (read_int(getPtr64(pit + j * 8), uid)) g_photo_players[g_photo_players_n++] = uid;
                    }
                }
            }
        }
    }
    g_photo_active = active;
    g_photo_present = (inst != 0);               // 有 PhotoUnit = 约瑟夫局(与拍没拍照无关)
    if (inst) snprintf(g_photo_diag, sizeof(g_photo_diag), "key=%d _time=%s _players=%d active=%d", g_photo_key, tbuf, players, (int)active);
    else      snprintf(g_photo_diag, sizeof(g_photo_diag), "%s", T("无 PhotoUnit(非约瑟夫局?)", "no PhotoUnit (not a Photographer match?)"));
}

static inline const char *photo_diag_text() { return g_photo_diag[0] ? g_photo_diag : T("(未扫描)", "(not scanned)"); }
// 我的 uid 在不在镜世界玩家列表里(只在 PySelf 线程调，与 scan 同线程、无竞态)
static bool self_in_players(int64_t uid)
{
    if (!g_photo_active.load()) return false;
    for (int i = 0; i < g_photo_players_n; i++) if (g_photo_players[i] == uid) return true;
    return false;
}

static bool try_refresh()
{
    if (g_module_dict == 0 && !resolve_module()) { g_in_lobby = false; return false; }

    Snapshot res;
    memset(&res, 0, sizeof(res));

    // ---- 零、units_by_type + 局外判定(必须在锚点之前，见 g_in_lobby) ----
    g_i_unit_mgr = find_key(g_module_dict, "unit_mgr", g_i_unit_mgr);
    uint64_t um = (g_i_unit_mgr >= 0) ? get_attr(g_module_dict, g_i_unit_mgr) : 0;
    uint64_t ud = get_inst_dict(um);
    uint64_t ubt = 0;
    if (ud) {
        g_i_ubt = find_key(ud, "units_by_type", g_i_ubt);
        ubt = (g_i_ubt >= 0) ? get_attr(ud, g_i_ubt) : 0;
    }
    bool in_lobby = false, has_copycat = true;   // 键表读不全时按"有"处理
    g_in_lobby = is_obj(ubt) && check_lobby(ubt, in_lobby, has_copycat) && in_lobby;
    if (is_obj(ubt)) scan_photo_unit(ubt, um);   // 约瑟夫镜世界激活信号(面板「镜世界:」)

    // 模仿者模式放在锚点之前：锚点失败时身份簿和坐标照样要更新(开会阶段锚点常读不到)
    static uint64_t pos_um = 0;
    if (is_obj(um) && um != pos_um) {                   // 换局：上一局的坐标、会议标志作废
        memset(g_pos_ms, 0, sizeof(g_pos_ms));
        memset(g_comp_cache, 0, sizeof(g_comp_cache));   // 组件缓存按 combat 单位地址记，换局地址会被复用
        g_meeting = false;
        g_puppet_op = false;
        pos_um = um;
    }
    if (is_obj(ubt) && has_copycat) collect_copycat(ubt, um, res);
    else {                                              // 不是模仿者局：食人鬼/尸体列表清空一次，免得残留上一局的
        if (g_extra_n[g_extra_active.load()] != 0) {
            int ew = 1 - g_extra_active.load();
            g_extra_n[ew] = 0;
            g_extra_active.store(ew);
        }
        if (g_corpse_n[g_corpse_active.load()] != 0) {
            int cwi = 1 - g_corpse_active.load();
            g_corpse_n[cwi] = 0;
            g_corpse_active.store(cwi);
        }
        if (g_stage[g_stage_active.load()].active) { StageView z; memset(&z, 0, sizeof(z)); stage_publish(z); }
        g_puppet_op = false;
    }
    pos_publish(now_ms());
    book_merge(res, um);

    // ---- 一、当前操控单位：g_cam_ctrl.unit ----
    g_i_cam = find_key(g_module_dict, "g_cam_ctrl", g_i_cam);
    uint64_t cam = (g_i_cam >= 0) ? get_attr(g_module_dict, g_i_cam) : 0;
    if (!is_obj(cam)) { snprintf(g_status, sizeof(g_status), "%s", T("g_cam_ctrl 无效(未在对局中?)", "g_cam_ctrl invalid (not in match?)")); return false; }

    uint64_t camd = get_inst_dict(cam);
    if (!camd) { snprintf(g_status, sizeof(g_status), "%s", T("g_cam_ctrl 不是 managed-dict 布局", "g_cam_ctrl is not managed-dict layout")); return false; }
    Dict camk;
    if (!read_dict(camd, camk)) { snprintf(g_status, sizeof(g_status), "%s", T("g_cam_ctrl 字典异常", "g_cam_ctrl dict malformed")); return false; }

    // 会议阶段：cam_handler 的类名。放在 cam.unit 之前，开会时锚点常读不到也照样更新
    g_i_cam_handler = resolve_index(camd, camk, g_i_cam_handler, "cam_handler");
    if (g_i_cam_handler >= 0) {
        uint64_t h = getPtr64(value_slot(camk, g_i_cam_handler));
        uint64_t tp = is_obj(h) ? getPtr64(h + 8) : 0;
        uint64_t np = is_obj(tp) ? getPtr64(tp + 0x18) : 0;     // PyTypeObject.tp_name
        char nm[48] = {0};
        if (is_obj(np) && vm_readv(np, nm, sizeof(nm) - 1)) {
            g_meeting = strcmp(nm, "CamModeCopycatMeeting") == 0;
            if (res.copycat_count > 0 && strcmp(nm, g_cam_mode_name) != 0) {   // 只在模仿者局打日志
                printf("[模仿者] 相机模式 %s 会议=%d\n", nm, (int)g_meeting.load());
                fflush(stdout);
            }
            memcpy(g_cam_mode_name, nm, sizeof(nm));
        }
    }

    g_i_cam_unit = resolve_index(camd, camk, g_i_cam_unit, "unit");
    if (g_i_cam_unit < 0) { snprintf(g_status, sizeof(g_status), "%s", T("g_cam_ctrl 里没有 unit", "no unit in g_cam_ctrl")); return false; }
    uint64_t me = getPtr64(value_slot(camk, g_i_cam_unit));
    if (!is_obj(me)) { snprintf(g_status, sizeof(g_status), "%s", T("cam.unit 为空", "cam.unit is null")); return false; }

    uint64_t me_d = get_inst_dict(me);
    if (!me_d) { snprintf(g_status, sizeof(g_status), "%s", T("cam.unit 没有实例字典", "cam.unit has no instance dict")); return false; }
    Dict me_k;
    if (!read_dict(me_d, me_k)) { snprintf(g_status, sizeof(g_status), "%s", T("cam.unit 字典异常", "cam.unit dict malformed")); return false; }

    g_i_model = resolve_index(me_d, me_k, g_i_model, "model");
    g_i_utype = resolve_index(me_d, me_k, g_i_utype, "unit_type");
    g_i_uid   = resolve_index(me_d, me_k, g_i_uid,   "uid");

    if (g_i_model >= 0) res.anchor = get_scene_obj(getPtr64(value_slot(me_k, g_i_model)));
    if (g_i_utype >= 0) {
        int64_t v = 0;
        if (read_int(getPtr64(value_slot(me_k, g_i_utype)), v)) {
            res.camp = (int)v;
            // 导演操控傀儡：在锚点之前发布(傀儡没真模型，锚点那步大概率失败)。读不到 unit_type 时沿用上次
            bool op = (v == COPYCAT_DIRECTOR_PUPPET);
            if (op != g_puppet_op.load()) {
                g_puppet_op = op;
                printf("[模仿者] 导演傀儡 %s\n", op ? "开始操控" : "结束操控");
                fflush(stdout);
            }
        }
    }
    if (g_i_uid   >= 0) { int64_t v = 0; if (read_int(getPtr64(value_slot(me_k, g_i_uid)),   v)) res.uid  = v; }

    if (res.anchor == 0) { snprintf(g_status, sizeof(g_status), "%s", T("cam.unit.model 读不到", "cam.unit.model unreadable")); return false; }

    // ---- 约瑟夫镜世界：我现在在不在镜世界 ----
    // 2026-10-22 坐实：= 我的 uid 在 PhotoUnit._players 里(进镜世界会把 uid append 进去，实测我进去后该列表 0→1)。
    // 比旧的读 anchor in_photo_world/out_photo_uid 权威。非约瑟夫局 photo_active=false → 恒 false，零副作用。
    res.in_mirror = self_in_players(res.uid);

    // ---- 二、废弃模型黑名单 + 本体集合 + 监管本体 ----
    if (is_obj(ubt))
        for (int i = 0; i < UNIT_TYPE_COUNT; i++) collect_units(ubt, UNIT_TYPES[i], res);
    book_mark_self(res.anchor);
    // 这一段读不到不算失败：锚点已经拿到了。本体数为 0 时绘制侧会退回按类名画

    int write_idx = 1 - g_active;
    g_buf[write_idx] = res;
    g_active = write_idx;                                   // 填完再翻转
    int pos_ok = 0;
    for (int i = 0; i < MAX_COPYCAT; i++) if (g_pos[g_pos_active][i].ok) pos_ok++;
    snprintf(g_status, sizeof(g_status), T("正常 %s uid=%lld 本体%d 废弃%d 身份%d 坐标%d%s", "OK %s uid=%lld bodies %d stale %d identities %d pos %d%s"),
             res.camp == 1 ? T("监管", "hunter") : (res.camp == 2 ? T("求生", "survivor") : (is_copycat_player_type(res.camp) ? T("模仿者局", "copycat") : T("从属", "minion"))),
             (long long)res.uid, res.body_count, res.stale_count, res.copycat_count,
             pos_ok, (res.copycat_count > 0 && g_pos_type.load() == 0) ? T("(未校准)", "(uncalibrated)") : "");
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
    if (++g_fail_streak >= 8) {                       // 连续失败就把序号缓存作废，下一轮按名字重找
        g_module_dict = 0;
        g_i_cam = g_i_cam_unit = g_i_unit_mgr = g_i_ubt = -1;
        g_i_model = g_i_another = g_i_utype = g_i_uid = -1;
        g_i_unit_model = g_i_cpuppet = -1;
        g_i_outphoto = -1;
        g_pd_i_time = g_pd_i_players = -1; g_photo_key = -1;
        g_cc_i_model = g_cc_i_another = g_cc_i_ident = g_cc_i_index = -1;
        g_cc_i_combat = g_cc_i_cdead = g_cc_i_cstealth = g_cc_i_ccomps = g_cc_i_soul = g_cc_i_chg = -1;
        memset(g_comp_cache, 0, sizeof(g_comp_cache));
        g_cc_i_gstate = g_cc_i_fstatus = g_cc_i_ftar = g_cc_i_dtarget = -1;
        g_stage_sys = 0;
        g_st_i_core = g_st_i_sysinst = g_st_i_in = g_st_i_wolf = g_st_i_center = g_st_i_rad = g_st_i_h = -1;
        g_i_cam_handler = -1;
        g_cc_i_uq = g_cp_i_uq = g_cp_i_reason = g_cp_i_combat = g_cp_i_clean = -1;
        for (int a = 0; a < 6; a++) g_cc_i_posattr[a] = -1;
        g_class_cache_count = 0;
        g_fail_streak = 0;
    }
}

// 自身锚点每帧都要用，而且切换操控对象时要立刻跟上，所以比天赋刷得勤(约一帧 60fps)
static const int REFRESH_INTERVAL_MS = 16;

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
    printf("[自身] 已启动 libbase=0x%lx\n", (unsigned long)libbase);
    fflush(stdout);
}

// ---------------- 给绘制侧用的只读接口 ----------------

static inline bool available() { return g_available; }
static inline const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

// 当前操控单位的场景对象。返回 false 时绘制侧就当没有自身(不猜)
static bool anchor(uint64_t &obj)
{
    if (!g_available) return false;
    uint64_t v = g_buf[g_active].anchor;
    if (v == 0) return false;
    obj = v;
    return true;
}

// 当前视角的阵营：1=监管 2=求生者。0=没读到。
// 注意操控从属单位时这里是从属的 unit_type(机械玩偶仍是 2、梦之信徒是 236)
static inline int camp() { return g_available ? g_buf[g_active].camp : 0; }
static inline int64_t self_uid() { return g_available ? g_buf[g_active].uid : 0; }

// 形态切换留下的废弃模型 —— 替掉红蝶/木偶师类名黑名单
static bool is_stale_model(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.stale_count; i++) if (s.stale[i] == (uint64_t)obj) return true;
    return false;
}

// 大厅/准备阶段：Python 侧还没有任何玩家单位，场景里的 chr/player、chr/boss 对象(时装挂件、头饰、袖子)
// 全都没有宿主，绘制侧据此整类不画。读不全时是 false，照旧按类名画
static inline bool in_lobby() { return g_in_lobby; }

// 本体集合能不能用。准备阶段/大厅里 units_by_type 没有 [1]/[2](实测两次)，这里就是 false，
// 绘制侧据此退回按类名画(读取失败时同样是 false，所以"是否局外"要看 in_lobby()，别用它)
static inline bool body_set_ready() { return g_available && g_buf[g_active].body_count > 0; }

static bool is_body(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.body_count; i++) if (s.body[i] == (uint64_t)obj) return true;
    return false;
}

// 约瑟夫镜世界：我当前在不在镜世界
static inline bool self_in_mirror() { return g_available && g_buf[g_active].in_mirror; }

// 这个本体是不是镜像(out_photo_uid 有值)。非本体 / 非约瑟夫局返回 false
static bool body_is_mirror(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.body_count; i++) if (s.body[i] == (uint64_t)obj) return s.body_mirror[i];
    return false;
}

// 这个本体是不是监管方单位(监管本体、巡视者、梦之信徒、伊斯幻影)。骨骼颜色用
static bool is_hunter_side(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.body_count; i++) if (s.body[i] == (uint64_t)obj) return s.body_hunter[i];
    return false;
}

// 约瑟夫镜世界：这个(真实)本体当前在不在镜世界(名字后缀 镜像/现实 用)。非本体/投影返回 false
static bool body_in_photo_world(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.body_count; i++) if (s.body[i] == (uint64_t)obj) return s.body_in_photo[i];
    return false;
}

// 当前是不是约瑟夫局(场上有 PhotoUnit)。约瑟夫局永远画框、永远不画骨骼用
static inline bool is_joseph_match() { return g_photo_present.load(); }

// 约瑟夫镜世界：这个本体是不是"另一个世界"的(相对我现在所在的世界)。
// 是 -> 绘制侧画成白色鬼魂、不画天赋。非约瑟夫局(没有镜像、in_mirror=false)恒 false。
static bool is_other_world(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.body_count; i++)
        if (s.body[i] == (uint64_t)obj) return s.body_mirror[i] != s.in_mirror;
    return false;
}

// 被监管扛在气球上的求生者(b_catch)。监管视角过滤掉糊在自己身上的那个框用。0 个时恒 false
static bool is_carried(uintptr_t obj)
{
    if (!g_available) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.carried_count; i++) if (s.carried[i] == (uint64_t)obj) return true;
    return false;
}

// 拷出本体集合(每个 = unit.model+0x20 场景对象，正好是 NativeBones 要的 M)。返回个数
static int copy_bodies(uint64_t *out, int cap)
{
    if (!g_available) return 0;
    const Snapshot &s = g_buf[g_active];
    int n = s.body_count < cap ? s.body_count : cap;
    for (int i = 0; i < n; i++) out[i] = s.body[i];
    return n;
}

// 局内监管本体的场景对象(跳过巡视者)。准备阶段没有 [1] 单位，返回 false
static bool hunter_body(uint64_t &obj)
{
    if (!g_available) return false;
    uint64_t v = g_buf[g_active].hunter_body;
    if (v == 0) return false;
    obj = v;
    return true;
}

// 自动盖板目标的场景对象：全部监管(联合狩猎 2 个)，梦之女巫换成被操控的信徒，
// 只返回跟我在同一个世界的(约瑟夫镜世界：游戏 check_in_same_world 不过就不判)。返回个数，准备阶段为 0
struct HunterTarget { uint64_t obj; bool armor; };
static int hunter_targets(HunterTarget *out, int cap)
{
    if (!g_available) return 0;
    const Snapshot &s = g_buf[g_active];
    int n = 0;
    for (int i = 0; i < s.hunter_count && n < cap; i++)
        if (s.hunter_in_photo[i] == s.in_mirror) out[n++] = {s.hunters[i], s.hunter_armor[i]};
    return n;
}

// 显示用的监管本体(联合狩猎 2 个；女巫只有本体)。返回个数
static int butcher_bodies(uint64_t *out, int cap)
{
    if (!g_available) return 0;
    const Snapshot &s = g_buf[g_active];
    int n = s.butcher_count < cap ? s.butcher_count : cap;
    for (int i = 0; i < n; i++) out[i] = s.butchers[i];
    return n;
}
static inline int butcher_count() { return g_available ? g_buf[g_active].butcher_count : 0; }
// 联合狩猎的队友监管：显示用本体里不是我自己的那个(我是女巫时，正在操控信徒的本体也算我自己)
static bool is_teammate_butcher(uintptr_t obj)
{
    if (!g_available || obj == 0) return false;
    const Snapshot &s = g_buf[g_active];
    if ((uint64_t)obj == s.self_butcher || (uint64_t)obj == s.anchor) return false;
    for (int i = 0; i < s.butcher_count; i++) if (s.butchers[i] == (uint64_t)obj) return true;
    return false;
}
static bool is_butcher_body(uintptr_t obj)
{
    if (!g_available || obj == 0) return false;
    const Snapshot &s = g_buf[g_active];
    for (int i = 0; i < s.butcher_count; i++) if (s.butchers[i] == (uint64_t)obj) return true;
    return false;
}

// ---- 模仿者模式 ----
// 当前是不是模仿者对局(1088 里读到了活人)
static inline bool copycat_active() { return g_available && g_buf[g_active].copycat_count > 0; }

// 会议阶段(相机在 CamModeCopycatMeeting)：绘制侧不画 in-world 人物，改画固定列表
static inline bool copycat_meeting() { return g_meeting.load(); }

// 我正在操控导演傀儡：绘制侧不画会议桌边的真人，改画已摆出的分身(见 DirectorView)
static inline bool director_puppet_mode() { return g_puppet_op.load(); }
static inline DirectorView director_view() { return g_dir[g_dir_active.load()]; }

// 指挥家舞台：这个坐标(x / 高度 / y)的人在不在台上、而且模型正被游戏挪到假位置(我在台上且不是模仿者阵营)
static bool in_fake_stage(float x, float z, float y)
{
    const StageView &s = g_stage[g_stage_active.load()];
    if (!s.active) return false;
    float dx = x - s.x, dy = y - s.y;
    return dx * dx + dy * dy <= s.radius * s.radius && fabsf(z - s.z) <= s.height;
}

// 界面开关「模仿者模式」每帧同步过来：开着才记身份簿，关掉就清空
static inline void set_copycat_book(bool on) { g_book_enabled = on; }

// 身份簿当前内容(拷贝一份给界面用，16 格很小)
static inline CopycatBook copycat_book() { return g_book[g_book_active]; }

// 簿子里有没有东西 —— 有就说明是模仿者局，绘制侧改用簿子过滤(开会时本体集合读不全)
static bool copycat_book_ready()
{
    const CopycatBook &b = g_book[g_book_active];
    for (int i = 0; i < MAX_COPYCAT; i++) if (b.seat[i].known && b.seat[i].body) return true;
    return false;
}

// 场景对象 -> 身份号(identity_id) 和 游戏里的号码。查身份簿，所以读不到的轮次沿用上次的值。
// 不是簿子里的活人(包括死者、假面舞会的石像等不认识的模型)返回 false
static bool copycat_identity(uintptr_t obj, int &identity, int &seat)
{
    const CopycatBook &b = g_book[g_book_active];
    for (int i = 0; i < MAX_COPYCAT; i++) {
        const CopycatSeat &s = b.seat[i];
        if (!s.known || s.dead || s.changing || s.boxed || s.body != (uint64_t)obj) continue;
        identity = s.identity;
        seat = i + 1;
        return true;
    }
    return false;
}

// 某个号码的人当前的 unit.position(x / 高度 / y)。没校准、没读到、超过 POS_HOLD_MS 没更新都返回 false，
// 绘制侧这时退回按场景对象画
static bool copycat_seat_pos(int seat, float &x, float &z, float &y)
{
    if (seat < 1 || seat > MAX_COPYCAT) return false;
    const CopycatPos &p = g_pos[g_pos_active][seat - 1];
    if (!p.ok) return false;
    x = p.x; z = p.z; y = p.y;
    return true;
}

// 额外单位(食人鬼 / 导演分身 / 导演傀儡)的当前快照：没号码没身份，绘制侧按 position 画、打固定标签
static int copycat_extra(ExtraUnit *out, int cap)
{
    int a = g_extra_active.load();
    int n = g_extra_n[a];
    if (n > cap) n = cap;
    memcpy(out, g_extra[a], sizeof(ExtraUnit) * n);
    return n;
}
// 尸体当前快照(x / 高度 / y / 号码，号码 0 = 没对上)
static int copycat_corpses(CorpseUnit *out, int cap)
{
    int a = g_corpse_active.load();
    int n = g_corpse_n[a];
    if (n > cap) n = cap;
    memcpy(out, g_corpse[a], sizeof(CorpseUnit) * n);
    return n;
}
static const char *const EXTRA_LABELS_EN[1] = { "Ghoul" };   // 游戏里叫"食尸鬼"，英文 Ghoul(TID_COPYCAT_TASK_NAME_19)
static inline const char *copycat_extra_label(uint8_t l) { return l < EXTRA_TYPE_COUNT ? (g_english ? EXTRA_LABELS_EN[l] : EXTRA_LABELS[l]) : "?"; }

// 阵营按号段：1xx 侦探团 / 2xx 模仿者 / 3xx 中立(没有独立的阵营字段，若辰那些 camp* 属性在这个模式下全不存在)
static inline int copycat_camp(int identity) { return identity / 100; }

// 身份名：若辰 sub_394ED4 的跳转表(本人两局 114/301 已实测对上)。
// 101 = 侦探：若辰先把名字预设成"侦探"再按 identity-101 查表，只有 101 那格直接跳到汇合点、不改名字(2026-09-26 解表确认)
static const char *copycat_identity_name(int identity)
{
    static const char *const good[] = { "侦探", "哨兵", "治安官", "猎人", "香料师", "锁匠", "银行家", "修士", "演说家",
                                        "拳击手", "灵媒", "学徒", "密探", "掮客", "药剂师", "巡林员", "执灯人", "评论家" };
    static const char *const bad[] = { "神偷", "千面人", "阴谋家", "烟火师", "怪盗", "催眠师", "地下医生", "处刑人", "指挥家" };
    static const char *const neutral[] = { "流浪汉", "送货员", "愚人", "棋手", "清洁工", "顾问", "降灵师", "导演" };
    // 英文：国际服 TID_COPYCAT_{DETECTIVE,COPYCAT,MYSTERIOUS}_NAME_<n>，序号与上面三张表一一对应(中文逐条核对过)
    static const char *const good_en[] = { "Detective", "Guard", "Sheriff", "Hunter", "Spice Maker", "Locksmith", "Banker", "Friar", "Orator",
                                           "Boxer", "Medium", "Apprentice", "Secret Agent", "Broker", "Pharmacist", "Ranger", "Patrol", "Critic" };
    static const char *const bad_en[] = { "Master Burglar", "Mimic", "Conspirator", "Pyrotechnist", "Phantom Thief", "Hypnotist",
                                          "Underground Doctor", "Executioner", "Conductor" };
    static const char *const neutral_en[] = { "Vagrant", "Courier", "Fool", "Chess Player", "Janitor", "Consultant", "Occultist", "Director" };
    int k = identity % 100 - 1;
    switch (identity / 100) {
        case 1: if (k >= 0 && k < (int)(sizeof(good) / sizeof(good[0]))) return g_english ? good_en[k] : good[k]; break;
        case 2: if (k >= 0 && k < (int)(sizeof(bad) / sizeof(bad[0]))) return g_english ? bad_en[k] : bad[k]; break;
        case 3: if (k >= 0 && k < (int)(sizeof(neutral) / sizeof(neutral[0]))) return g_english ? neutral_en[k] : neutral[k]; break;
    }
    return nullptr;
}

} // namespace PySelf

#endif // IDV_PY_SELF_H
