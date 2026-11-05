#include "draw.h"
#include <thread>
#include <cstdint>
#include <stdio.h>
#include "My_font/wrg_font.h"
#include "kerneldriver-qxqd.hpp"
#include "DrawTool.h"
#include "Name.h"
#include "SoHookIntegration.h"
#include "PyProgress.h"
#include "PyGenius.h"
#include "PySelf.h"
#include "PyScene.h"
#include "AutoPallet.h"
#include "NativeBones.h"
#include "GeniusTree.h"
#include "PyWaterFight.h"
#include "PyItems.h"
#include "WaxAim.h"
#include <linux/input.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <vector>
#include <algorithm>
std::string filter_class_name;
// 类名缓存：对象地址 -> {第一跳指针(校验用), 类名}。只有读线程碰它，不用加锁。见读取循环里的说明
static std::unordered_map<uintptr_t, std::pair<uint64_t, std::string>> g_name_cache;
float dist_scale=11.886;
float redqueen_x, redqueen_y, redqueen_z;
float redqueen_mirror_x, redqueen_mirror_y, redqueen_mirror_z;
typedef struct {
    uintptr_t obj;
    uintptr_t objcoor;
    int camp;
    char str[256];//翻译名
    char class_name[256];//类名
}DataStruct;
DataStruct data[1000];

bool permeate_record = false;
bool permeate_record_ini = false;
struct Last_ImRect LastCoordinate = {0, 0, 0, 0};
static uint32_t orientation = -1;
ANativeWindow *window; 
// 屏幕信息
android::ANativeWindowCreator::DisplayInfo displayInfo;
// 窗口信息
ImGuiWindow *g_window;
// 绝对屏幕X _ Y
int abs_ScreenX, abs_ScreenY;
int native_window_screen_x, native_window_screen_y;
std::unique_ptr<AndroidImgui>  graphics;
ImFont* zh_font = NULL;
bool niexi;
float cam_dist;
float niexi_dist,niexi_hold_dist;
/*定义*/
bool DrawIo[50];
float niexi_touch_x,niexi_touch_y;
bool M_Android_LoadFont(float SizePixels) {
    ImGuiIO &io = ImGui::GetIO();
    
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 1;
    config.SizePixels = SizePixels;
    ::zh_font = io.Fonts->AddFontFromMemoryTTF((void *)WRG_Font, WRG_Font_size, SizePixels, &config, io.Fonts->GetGlyphRangesChineseFull());

    return zh_font != nullptr;
}
void init_My_drawdata() {
    M_Android_LoadFont(25.0f); //加载内存字体(含中文TTF+图标)
}


void screen_config() {
    ::displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
}

void drawBegin() {
    if (::permeate_record_ini) {
        LastCoordinate.Pos_x = ::g_window->Pos.x;
        LastCoordinate.Pos_y = ::g_window->Pos.y;
        LastCoordinate.Size_x = ::g_window->Size.x;
        LastCoordinate.Size_y = ::g_window->Size.y;

        graphics->Shutdown();
        android::ANativeWindowCreator::Destroy(::window);
        ::window = android::ANativeWindowCreator::Create("逆天改命", native_window_screen_x, native_window_screen_y, permeate_record);
        graphics->Init_Render(::window, native_window_screen_x, native_window_screen_y);
        ::init_My_drawdata(); //初始化绘制数据
    } 

    screen_config();
    if (::orientation != displayInfo.orientation) {
        ::orientation = displayInfo.orientation;
        Touch::setOrientation(displayInfo.orientation);
        if (g_window != NULL) {
            g_window->Pos.x = 100;
            g_window->Pos.y = 125;        
        }        
        //cout << " width:" << displayInfo.width << " height:" << displayInfo.height << " orientation:" << displayInfo.orientation << endl;
    }
}

struct Vector3A
{
	float X;
	float Y;
	float Z;

	  Vector3A()
	{
		this->X = 0;
		this->Y = 0;
		this->Z = 0;
	}

	Vector3A(float x, float y, float z)
	{
		this->X = x;
		this->Y = y;
		this->Z = z;
	}

};

float xs_final, ys_final;
void calculate_line_reflection(float x1, float y1, float x2, float y2, float xs, float ys, float *xs_prime, float *ys_prime) {
    float A = y2 - y1;
    float B = x1 - x2;
    float C = x2 * y1 - x1 * y2;

    float D = A * xs + B * ys + C;
    float denom = A * A + B * B;

    *xs_prime = xs - 2.0 * A * D / denom;
    *ys_prime = ys - 2.0 * B * D / denom;
}


uintptr_t libbase;
uintptr_t Arrayaddr, Count, Matrix;
uintptr_t cur_obj,self_obj,self_camp,namezfcz,namezfc;
uintptr_t redqueen_obj,redqueen_mirror_obj,mirror_obj,mirror_preview_obj;
float mirror_line_x1, mirror_line_y1, mirror_line_x2, mirror_line_y2;   // 本帧镜面在水平面上的直线(两点)，mirror==true 时有效
int data_count,zfcz,zfc;
float matrix[16];
float angle;

static bool show_draw_Rect = false;//方框
static bool show_draw_Bones = true;//原生骨骼(抄若辰链,驱动读)
static bool show_draw_Line = true;//射线
static bool show_draw_Camera = false;//相机
static bool show_draw_Door = true;//门(开门进度，走 Python 层单独绘制，不依赖 getscene)
static bool show_draw_Box = false;//盒子
static bool show_draw_Name = true;//名字
static bool show_draw_Distance = true;//距离
static bool show_draw_Cellar = true;//地窖
static bool show_draw_Chair = false;//椅子
static bool show_draw_Prop = false;//道具
static bool show_draw_Genius = true;//天赋/辅助特质(走 CPython 链路，见 PyGenius.h)
static bool show_draw_prophet = true;//预知监管者
static bool redqueenmod = false;//红夫人模式
static bool copycat_mode = false;//模仿者模式：角色名换成"N号 身份"，按阵营着色(见 PySelf.h 模仿者模式那节)
static bool waterfight_mode = false;//林中童话(娱乐模式,底层打水仗)：独立绘制玩家(动物阵营配色)+物资，见 PyWaterFight.h
static bool show_draw_secret_mechine = true;//密码机(含破译进度/进度条/最后一台)
static bool show_draw_Role = false;//角色
static bool show_draw_touch = false;//孽蜥
static bool Debugging = false;//调试
static bool mirror = false;//镜子状态
static bool show_demo_window = false;
static bool show_another_window = false;
static bool show_window = true;  // 音量键控制：音量下=隐藏，音量上=显示
static bool voice = true;
static bool inform_ghost = true; // 显示鬼魂
static bool show_sohook = false;  // 骨骼与进度覆盖层
static bool show_genius_tree = false;  // 监管天赋树(径向图，独立可拖动窗口)
static bool show_crosshair = false;    // 准星(所有角色通用)，位置 = WaxAim::aim_x/aim_y
static bool shot_hide_menu = true;     // 截图时先把主菜单藏起来
// 准星位置按角色(pid)分别记：配置文件里每个角色一行 xhair=pid,x,y。换角色时自动套用；
// 这个角色没存过就用蜡像师(pid 112)的，蜡像师也没有就用默认(游戏蜡枪标记，截图实测 2772x1280 下 (1385.5, 556.6))
#include <map>
static const float WAX_MARK_Y = 556.6f / 1280.f;
// 准星：位置 + 样式 + 大小(臂长，像素) + 粗细(像素)，整套按角色记。x/y < 0 表示"没设"
enum { XH_CROSS_DOT = 0, XH_CROSS, XH_DOT, XH_CIRCLE, XH_CIRCLE_DOT, XH_T, XH_STYLE_N };
struct XhairCfg { float x = -1.f, y = -1.f; int style = XH_CROSS_DOT; float size = 12.f, thick = 2.f; };
static std::map<int, XhairCfg> g_xhair;
static XhairCfg g_xh;                                          // 当前套用中的(位置同步在 WaxAim::aim_x/aim_y，蜡像师瞄准线用)
static int g_xhair_pid = -2;                                   // 当前已套用的角色，-2 = 需要重新套用
static float panel_alpha = 0.94f;      // 菜单/天赋树窗口背景不透明度(同金铲铲 ui.alpha)。0.94 = ImGui 默认 WindowBg

// ===== 固定信息：屏幕坐标 HUD 汇总，不依赖矩阵。每块独立开关 + 独立 X/Y/大小(size=文字像素高) =====
//   调试阶段用菜单里的滑动条摆位置/大小，调好后把下面的初值固化即可。
//   x<=0 的块(监管特质默认)表示"水平居中"，用 px 算。
static bool  fi_gen_on    = true;                                        // 密码机/大门 汇总面板(左中)
static float fi_gen_x     = 40.0f,   fi_gen_y    = 320.0f, fi_gen_size    = 25.0f;
static bool  fi_hunter_on = true;                                        // 监管 辅助特质 + 大天赋(顶中，只在我是求生者时显示)
static float fi_hunter_x  = 0.0f,    fi_hunter_y = 173.0f, fi_hunter_size = 30.0f;
static bool  fi_civ_on    = true;                                        // 求生者天赋(右上横排，只在我是监管时显示)
static float fi_civ_x     = 1894.0f, fi_civ_y    = 328.0f, fi_civ_size    = 28.0f, fi_civ_gap = 80.0f;

// ===== 配置保存/读取：/data/IdentityV_config.txt，一行一个 key=value(# 开头是注释，可手改) =====
// 读取时未知键忽略、缺的键保持当前值，所以加了新选项旧文件照样能读。
// 窗口位置/大小：每帧记下当前值用于保存；读取后下一帧用 SetNextWindowPos/Size 套上。
// 启动后第一帧自动读一次(文件不存在就用默认值，不报错)；读之前先把各项的默认值拍一份快照，
// 「恢复默认配置」从快照还原(只改内存，要写回文件再点保存)。
#include <cerrno>
#include <cstring>
static const char *CONFIG_PATH = "/data/IdentityV_config.txt";
static float cfg_main_x = -1.f, cfg_main_y = -1.f;                          // 主菜单位置
static float cfg_tree_x = -1.f, cfg_tree_y = -1.f, cfg_tree_w = -1.f, cfg_tree_h = -1.f;   // 天赋树窗口
static bool  cfg_apply_main = false, cfg_apply_tree = false;               // 读取后待套用
static char  cfg_msg[160] = "";
static double cfg_msg_time = -100.0;

enum CfgType { CFG_BOOL, CFG_INT, CFG_FLOAT };
struct CfgItem { const char *key; CfgType type; void *ptr; };
static const CfgItem CFG_ITEMS[] = {
    {"english", CFG_BOOL, &g_english},                {"panel_alpha", CFG_FLOAT, &panel_alpha},
    // 绘制设置
    {"show_rect", CFG_BOOL, &show_draw_Rect},          {"show_bones", CFG_BOOL, &show_draw_Bones},
    {"show_line", CFG_BOOL, &show_draw_Line},          {"show_camera", CFG_BOOL, &show_draw_Camera},
    {"show_door", CFG_BOOL, &show_draw_Door},          {"show_box", CFG_BOOL, &show_draw_Box},
    {"show_name", CFG_BOOL, &show_draw_Name},          {"show_distance", CFG_BOOL, &show_draw_Distance},
    {"show_cellar", CFG_BOOL, &show_draw_Cellar},      {"show_chair", CFG_BOOL, &show_draw_Chair},
    {"show_prop", CFG_BOOL, &show_draw_Prop},          {"show_genius", CFG_BOOL, &show_draw_Genius},
    {"show_prophet", CFG_BOOL, &show_draw_prophet},    {"redqueen_mode", CFG_BOOL, &redqueenmod},
    {"show_cipher", CFG_BOOL, &show_draw_secret_mechine}, {"show_role", CFG_BOOL, &show_draw_Role},
    {"debug", CFG_BOOL, &Debugging},                   {"show_ghost", CFG_BOOL, &inform_ghost},
    {"show_genius_tree", CFG_BOOL, &show_genius_tree},
    // 娱乐模式
    {"copycat_mode", CFG_BOOL, &copycat_mode},         {"waterfight_mode", CFG_BOOL, &waterfight_mode},
    // 固定信息
    {"fi_gen_on", CFG_BOOL, &fi_gen_on},     {"fi_gen_x", CFG_FLOAT, &fi_gen_x},     {"fi_gen_y", CFG_FLOAT, &fi_gen_y},     {"fi_gen_size", CFG_FLOAT, &fi_gen_size},
    {"fi_hunter_on", CFG_BOOL, &fi_hunter_on}, {"fi_hunter_x", CFG_FLOAT, &fi_hunter_x}, {"fi_hunter_y", CFG_FLOAT, &fi_hunter_y}, {"fi_hunter_size", CFG_FLOAT, &fi_hunter_size},
    {"fi_civ_on", CFG_BOOL, &fi_civ_on},     {"fi_civ_x", CFG_FLOAT, &fi_civ_x},     {"fi_civ_y", CFG_FLOAT, &fi_civ_y},     {"fi_civ_size", CFG_FLOAT, &fi_civ_size},
    {"fi_civ_gap", CFG_FLOAT, &fi_civ_gap},
    // 蜡像师
    {"wax_on", CFG_BOOL, &WaxAim::enabled},      {"wax_traj", CFG_BOOL, &WaxAim::show_traj},
    {"wax_always", CFG_BOOL, &WaxAim::show_always}, {"show_crosshair", CFG_BOOL, &show_crosshair},
    {"shot_hide_menu", CFG_BOOL, &shot_hide_menu},
    // 自动盖板
    {"pallet_enabled", CFG_BOOL, &AutoPallet::enabled},           {"pallet_show_range", CFG_BOOL, &AutoPallet::show_range},
    {"pallet_show_touch", CFG_BOOL, &AutoPallet::show_touch_point}, {"pallet_touch_x", CFG_FLOAT, &AutoPallet::touch_x},
    {"pallet_touch_y", CFG_FLOAT, &AutoPallet::touch_y},          {"pallet_hold_ms", CFG_INT, &AutoPallet::hold_ms},
    {"pallet_jitter_px", CFG_INT, &AutoPallet::jitter_px},        {"pallet_jitter_ms", CFG_INT, &AutoPallet::jitter_ms},
    {"pallet_predict", CFG_BOOL, &AutoPallet::predict_fast},      {"pallet_predict_mps", CFG_FLOAT, &AutoPallet::fast_mps},
    {"pallet_lead_ms", CFG_INT, &AutoPallet::lead_ms},
    {"pallet_width", CFG_FLOAT, &AutoPallet::width},              {"pallet_random", CFG_BOOL, &AutoPallet::random_width},
    {"pallet_width_lo", CFG_FLOAT, &AutoPallet::width_lo},        {"pallet_width_hi", CFG_FLOAT, &AutoPallet::width_hi},    // 窗口
    {"win_main_x", CFG_FLOAT, &cfg_main_x}, {"win_main_y", CFG_FLOAT, &cfg_main_y},
    {"win_tree_x", CFG_FLOAT, &cfg_tree_x}, {"win_tree_y", CFG_FLOAT, &cfg_tree_y},
    {"win_tree_w", CFG_FLOAT, &cfg_tree_w}, {"win_tree_h", CFG_FLOAT, &cfg_tree_h},
};

static void cfg_set_msg(const char *s) { snprintf(cfg_msg, sizeof(cfg_msg), "%s", s); cfg_msg_time = ImGui::GetTime(); }
static void cfg_mark_saved();   // 定义在下面(要用 CFG_COUNT)

static void config_save()
{
    FILE *f = fopen(CONFIG_PATH, "w");
    char m[160];
    if (!f) { snprintf(m, sizeof(m), T("保存失败: %s", "Save failed: %s"), strerror(errno)); cfg_set_msg(m); return; }
    fprintf(f, "# IdentityV overlay config. key=value, one per line. Unknown keys are ignored.\n");
    for (const CfgItem &it : CFG_ITEMS) {
        switch (it.type) {
            case CFG_BOOL:  fprintf(f, "%s=%d\n", it.key, *(bool *)it.ptr ? 1 : 0); break;
            case CFG_INT:   fprintf(f, "%s=%d\n", it.key, *(int *)it.ptr); break;
            case CFG_FLOAT: fprintf(f, "%s=%.3f\n", it.key, *(float *)it.ptr); break;
        }
    }
    if (g_xhair_pid > 0 && WaxAim::aim_x >= 0 && WaxAim::aim_y >= 0) {        // 当前角色的准星一并记下
        g_xh.x = WaxAim::aim_x; g_xh.y = WaxAim::aim_y;
        g_xhair[g_xhair_pid] = g_xh;
    }
    for (auto &kv : g_xhair)                                                  // xhair=角色pid,x,y,样式,大小,粗细
        fprintf(f, "xhair=%d,%.1f,%.1f,%d,%.1f,%.1f\n", kv.first, kv.second.x, kv.second.y, kv.second.style, kv.second.size, kv.second.thick);
    bool ok = fflush(f) == 0;
    ok = (fclose(f) == 0) && ok;
    if (ok) { snprintf(m, sizeof(m), T("已保存 %d 项 → %s", "Saved %d items -> %s"), (int)(sizeof(CFG_ITEMS) / sizeof(CFG_ITEMS[0])), CONFIG_PATH); cfg_mark_saved(); }
    else    snprintf(m, sizeof(m), T("保存失败: %s", "Save failed: %s"), strerror(errno));
    cfg_set_msg(m);
}

// 默认值快照：CfgItem 只有 bool/int/float 三种，4 字节装得下
static const int CFG_COUNT = (int)(sizeof(CFG_ITEMS) / sizeof(CFG_ITEMS[0]));
static uint32_t cfg_defaults[CFG_COUNT];

static void config_snapshot_defaults()
{
    for (int i = 0; i < CFG_COUNT; i++) {
        const CfgItem &it = CFG_ITEMS[i];
        cfg_defaults[i] = 0;
        memcpy(&cfg_defaults[i], it.ptr, it.type == CFG_BOOL ? sizeof(bool) : 4);
    }
}

static void config_restore_defaults()
{
    for (int i = 0; i < CFG_COUNT; i++) {
        const CfgItem &it = CFG_ITEMS[i];
        memcpy(it.ptr, &cfg_defaults[i], it.type == CFG_BOOL ? sizeof(bool) : 4);
    }
    cfg_set_msg(T("已恢复默认(要写回文件请再点保存)", "Defaults restored (click Save to write the file)"));
}

// 未保存标记：记下"最后一次保存/读取时"的值，每帧比对，不同就在保存按钮上加 *。
// 窗口位置(win_*)不比：拖一下窗口就变、首次运行没有文件时也一定不同，标出来只会一直亮着
static uint32_t cfg_saved[CFG_COUNT];
static std::map<int, XhairCfg> cfg_saved_xhair;

static void cfg_mark_saved()
{
    for (int i = 0; i < CFG_COUNT; i++) {
        const CfgItem &it = CFG_ITEMS[i];
        cfg_saved[i] = 0;
        memcpy(&cfg_saved[i], it.ptr, it.type == CFG_BOOL ? sizeof(bool) : 4);
    }
    cfg_saved_xhair = g_xhair;
}

static bool cfg_dirty()
{
    for (int i = 0; i < CFG_COUNT; i++) {
        const CfgItem &it = CFG_ITEMS[i];
        if (strncmp(it.key, "win_", 4) == 0) continue;
        uint32_t v = 0;
        memcpy(&v, it.ptr, it.type == CFG_BOOL ? sizeof(bool) : 4);
        if (v != cfg_saved[i]) return true;
    }
    if (g_xhair.size() != cfg_saved_xhair.size()) return true;
    for (auto &kv : g_xhair) {
        auto f = cfg_saved_xhair.find(kv.first);
        if (f == cfg_saved_xhair.end() || memcmp(&f->second, &kv.second, sizeof(XhairCfg)) != 0) return true;
    }
    return false;
}

static void config_load(bool startup = false)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    char m[160];
    if (!f) {
        if (startup && errno == ENOENT) return;   // 首次运行还没有配置文件，静默用默认值
        snprintf(m, sizeof(m), T("读取失败: %s", "Load failed: %s"), strerror(errno)); cfg_set_msg(m); return;
    }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *val = eq + 1;
        if (strcmp(line, "xhair") == 0) {                               // 每个角色的准星(旧行只有 pid,x,y，样式/大小/粗细取默认)
            int pid = 0; XhairCfg c;
            if (sscanf(val, "%d,%f,%f,%d,%f,%f", &pid, &c.x, &c.y, &c.style, &c.size, &c.thick) >= 3 && pid > 0) {
                if (c.style < 0 || c.style >= XH_STYLE_N) c.style = XH_CROSS_DOT;
                g_xhair[pid] = c; n++;
            }
            continue;
        }
        if (strcmp(line, "wax_aim_x") == 0 || strcmp(line, "wax_aim_y") == 0) {   // 旧版只有蜡像师一套，迁到 pid 112
            float v = (float)atof(val);
            XhairCfg &c = g_xhair[WaxAim::WAX_PID];
            if (line[8] == 'x') c.x = v; else c.y = v;
            n++;
            continue;
        }
        for (const CfgItem &it : CFG_ITEMS) {
            if (strcmp(it.key, line) != 0) continue;
            switch (it.type) {
                case CFG_BOOL:  *(bool *)it.ptr = atoi(val) != 0; break;
                case CFG_INT:   *(int *)it.ptr = atoi(val); break;
                case CFG_FLOAT: *(float *)it.ptr = (float)atof(val); break;
            }
            n++;
            break;
        }
    }
    fclose(f);
    g_xhair_pid = -2;                                                   // 读完按当前角色重新套用准星位置
    if (panel_alpha < 0.10f) panel_alpha = 0.10f;
    if (panel_alpha > 1.0f)  panel_alpha = 1.0f;
    cfg_apply_main = cfg_main_x >= 0.f && cfg_main_y >= 0.f;
    cfg_apply_tree = cfg_tree_w > 0.f && cfg_tree_h > 0.f;
    snprintf(m, sizeof(m), T("已读取 %d 项 ← %s", "Loaded %d items <- %s"), n, CONFIG_PATH);
    cfg_set_msg(m);
    cfg_mark_saved();
}

float z_x, z_y, z_z, d_x, d_y, d_z, camera, r_x, r_y, r_w;
float X1,Y1,X2,Y2,W,H,MIDDLE,TOP,BOTTOM;
float dist;   // 2026-09-23: int -> float。以前的截断迫使两处亚米级判断另外重算一遍
char objtext[256];
//char content[1024];
char Team[1024];
char Name[1024];
char prophet_text[1024];
int g_room_hunter_n = -1;   // 准备房间里的监管人数(UIRoomWait.b_uid2pid 条目数)，-1 = 不在准备阶段/读不到。面板显示用

// 预知文本里追加一个监管名(已经有了就不加)。联合狩猎两个监管时拼成 "[杰克] [小丑]"
static void prophet_append(char *buf, size_t cap, const char *name)
{
    if (!name || !name[0] || strstr(buf, name)) return;
    size_t len = strlen(buf);
    snprintf(buf + len, cap - len, "%s%s", len ? " " : "", name);
}

float px,py;
Vector3A D,Z;


// 监管天赋 id(1~40) -> 名称 / 最大等级。来自游戏配表 GK.DM.butcher_genius，
// 探针 cmd_hunter_tree.py 实测逐条对齐(idv-replay-genius/genius-table.md)。下标 0 占位。
static const char *const BUTCHER_GENIUS_NAME[41] = {
    "",
    "恶化","执念","枯萎","恐慌","破坏欲","狂暴","愤怒","禁闭空间",
    "封禁","困兽之斗","狩猎本能","报幕","好客之道","崩坏","通缉","底牌",
    "惯性","摧枯拉朽","嘲弄","临界点","狂欢","巨钳","拘禁狂","挽留",
    "狩鹿","掌控欲","耐受力","表现欲","后遗症","清道夫","追猎","张狂",
    "嬉弄","幽闭恐惧","冲动","成瘾症","余威","饥荒","惊弓之鸟","念旧癖"
};
// 英文全名：国际服热更后的 TID_BUTCHER_NAME_*_LV1(离线解包，中文逐条唯一对上，见解包存档 SKILL §7.8)。
// ⚠ apk 原包里 id 20/37/39 是旧天赋(猫鼠游戏/警觉/无人生还)，必须用 Documents/res 热更那份
[[maybe_unused]] static const char *const BUTCHER_GENIUS_NAME_EN[41] = {   // 目前只用缩写；要显示全名时用它
    "",
    "Deteriorate","Obsession","Wither","Panic","Destructiveness","Berserker","Rage","Confined Space",
    "Constrain","Desperate Fight","Hunter's Instinct","Announcement","Hospitable","Impact","Wanted Order","Trump Card",
    "Inertia","Overwhelm","Mock","Tipping Point","Carnival","Giant Claw","Sadist","Detention",
    "Deer Hunt","Control Freak","Tolerance","Exhibitionist","After Effects","Street Sweeper","Hunt","Insolence",
    "Mischievous","Claustrophobia","Impulsive","Addiction","Lingering Might","Famine","Skittish","Nostalgic"
};
// 天赋树节点圆圈里放不下全名：中文取前 2 字，英文用这张缩写表(两两不重复)
static const char *const BUTCHER_GENIUS_ABBR_EN[41] = {
    "",
    "Detr","Obs","Wthr","Pnc","Dstr","Bsk","Rage","CS",
    "Cstr","DF","HI","Ann","Hosp","Imp","WO","TC",
    "Inrt","Ovw","Mock","TP","Carn","GC","Sad","Det",
    "DH","CF","Tol","Exh","AE","SS","Hunt","Ins",
    "Msch","Clau","Impl","Add","LM","Fam","Skt","Nost"
};
static const unsigned char BUTCHER_GENIUS_MAXLV[41] = {
    0,
    1,1,3,1,3,3,3,1,   1,1,3,1,3,3,3,1,
    1,1,3,1,3,3,3,1,   1,1,3,1,3,3,3,1,
    1,1,1,1,1,1,1,1
};
static inline bool is_terminal_genius(int id){ return id==8||id==16||id==24||id==32; }

ImColor color_red = ImColor(255,0,0,255);
ImColor color_green = ImColor(0,255,0,255);
ImColor color_blue = ImColor(0,0,255,255);
ImColor color_yellow = ImColor(255,255,0,255);
ImColor color_purple = ImColor(255,0,255,255);
ImColor color_black = ImColor(0,0,0,255);
ImColor BoneColor = ImColor(255,0,0,255);
ImColor BotBoneColor = ImColor(255,255,255,255);
int read_state = 0;
bool first_frame_logged = false;
bool first_matrix_logged = false;
char extractedString[64];
long int MatrixOffset = 0,ArrayaddrOffset = 0;
typedef struct {
    unsigned long addr;
    unsigned long taddr;
} ModuleBssInfo;


ModuleBssInfo get_module_bss(int pid, const char *module_name) {
    FILE *fp;
    ModuleBssInfo info = {0, 0};
    char filename[64];
    char line[1024];

    // 生成文件名
    snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);

    // 打开文件
    fp = fopen(filename, "r");

    bool found_module = false;

    if (fp!= NULL) {
        while (fgets(line, sizeof(line), fp)) {
            // 先判断是否包含模块名
            if (strstr(line, module_name)!= NULL) {
                found_module = true;
            }

            if (found_module) {
                // 检查是否满足rw权限且行长度符合要求
                long addr,taddr;
                sscanf(line, "%lx-%lx", &addr, &taddr);
                if (strstr(line, "rw")!= NULL && strlen(line) < 86 &&(taddr-addr)/4096>=2800) {
                //printf("%d", (taddr-addr)/4096);
                
                    // 将行按空格分割成字符串数组（这里简单示意，实际可能需要更完善的分割函数）
                    char *words[10];
                    int numWords = 0;
                    char *token = strtok(line, " ");
                    while (token!= NULL && numWords < 10) {
                        words[numWords++] = token;
                        token = strtok(NULL, " ");
                    }

                    // 遍历分割后的字符串数组，查找地址范围并转换
                    for (int i = 0; i < numWords; i++) {
                        if (sscanf(words[i], "%lx-%lx", &info.addr, &info.taddr) == 2) {
                            fclose(fp);
                            return info;
                        }
                    }

                    // 如果未找到正确格式的地址范围，设置为0并返回
                    info.addr = 0;
                    info.taddr = 0;
                    fclose(fp);
                    return info;
                }
            }
        }

        fclose(fp);
    }

    return info;
}

ModuleBssInfo get_module_bssgjf(int pid, const char *module_name) {
    FILE *fp;
    ModuleBssInfo info = {0, 0};
    long addr,taddr;
    char *pch;
    char filename[64];
    char line[1024];
    snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    fp = fopen(filename, "r");
    bool is = false;
    if (fp!= NULL) {
        while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "%lx-%lx", &addr, &taddr);
            // 国际服引擎 r-xp 大小不是国服的 114982912(实测 161984512,且会随版本变),
            // 改按包名 com.netease.idv + 巨型段(>80MB)锁定引擎,随后取其后的 [anon:.bss] 段。
            if (strstr(line, "com.netease.idv") && strstr(line, "r-xp") != NULL && (taddr-addr) > 0x5000000) {
                is = true;
            }
            if (is) {
                if (strstr(line, "rw")!= NULL &&!feof(fp) && (strlen(line) < 86)) {
                long addr,taddr;
                sscanf(line, "%lx-%lx", &addr, &taddr);
                if ((taddr-addr)/4096<=3000)
                continue;
                    if (sscanf(line, "%lx-%lx", &info.addr, &info.taddr)!= 2) {
                        // 处理转换失败的情况
                        info.addr = 0;
                        info.taddr = 0;
                        break;
                    }
                    break;
                }
            }
        }
        fclose(fp);
    }
    return info;
}
// 本地化 Text 助手：ImGui::Text/TextColored 带 printf format 属性，直接喂非字面量格式串会触发
// -Wformat-security。这里先按 g_english 选中/英格式串,vsnprintf 到缓冲,再以字面量 "%s" 输出。
#include <cstdarg>
static void ImTextT(const char* cn, const char* en, ...) {
    char buf[256];
    va_list ap; va_start(ap, en);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(buf, sizeof(buf), g_english ? en : cn, ap);
#pragma clang diagnostic pop
    va_end(ap);
    ImGui::TextUnformatted(buf);
}
static void ImTextColoredT(const ImVec4& col, const char* cn, const char* en, ...) {
    char buf[256];
    va_list ap; va_start(ap, en);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(buf, sizeof(buf), g_english ? en : cn, ap);
#pragma clang diagnostic pop
    va_end(ap);
    ImGui::TextColored(col, "%s", buf);
}

int get_name_pid1(const char *packageName) {
    int id = -1;
    DIR *dir;
    FILE *fp;
    char filename[64];
    char cmdline[64] = {};
    struct dirent *entry;
    dir = opendir("/proc");
    if (dir == NULL) {
        return -1;
    }
    while ((entry = readdir(dir))!= NULL) {
        id = atoi(entry->d_name);
        if (id!= 0) {
            sprintf(filename, "/proc/%d/cmdline", id);
            fp = fopen(filename, "r");
            if (fp) {
                char *readResult = fgets(cmdline, sizeof(cmdline), fp);
                fclose(fp);
                if (readResult != NULL &&
                    (strstr(cmdline, packageName) != NULL || strstr(cmdline, "com.netease.idv") != NULL) &&
                    strstr(cmdline, "com") != NULL && strstr(cmdline, "PushService") == NULL &&
                    strstr(cmdline, "gcsdk") == NULL) {
                    sprintf(extractedString, "%s", cmdline);
                    closedir(dir);
                    return id;
                }
            }
        }
    }
    closedir(dir);
    return -1;
}
long getModuleBasegjf(int pid, const char *module_name) {
    FILE *fp;
    long addr,taddr;
    char *pch;
    char filename[64];
    char line[1024];
    snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    fp = fopen(filename, "r");
    bool is = false;
    if (fp!= NULL) {
        while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "r-xp")!= NULL &&!feof(fp) && strstr(line, module_name)) {
                sscanf(line, "%lx-%lx", &addr, &taddr);
                    if ((taddr-addr)== 114982912) {
                        // 处理转换失败的情况
                        fclose(fp);
                        return addr;
                        break;
                    }
                    //break;
                }
            
        }
        fclose(fp);
    }
    return 0;
}

char libso[256] = {"libclient.so"};

// ==================== 过滤表 ====================
static const char* g_filter_keywords[] = {
    "creature",
    "dm65_survivor_girl_page",
    "skill_hudie",
    "h55_joseph_camera",
    "burke_console",
    "redqueen_e_heijin_yizi",
    "qiutu_box",
    "weapon",
    "nvyao"
};
static constexpr int g_filter_count = sizeof(g_filter_keywords) / sizeof(g_filter_keywords[0]);
inline bool should_filter(const std::string& name) {
    for (int i = 0; i < g_filter_count; ++i) {
        if (name.find(g_filter_keywords[i]) != std::string::npos)
            return true;
    }
    return false;
}

// ================== 幽灵/隐身状态判定(合并原有特殊场景排除) ==================
// +0x70 == 0x1000000 且 +0x1a0 == 450.0 才是"正常在场"的真实角色/道具;
// 其余取值(包括65150鬼魂视角等)统一视为幽灵态, 由 inform_ghost 决定是否仍然显示。
// 方框配色也用这一个判据 —— 热更后鬼魂的 +0x70 不再是 65150, 只认 65150 会让鬼魂框不变白。
bool IsGhostEntity(const DataStruct& obj) {
    int checkVal = getDword(obj.obj + 0x70);
    float checkFloat = getFloat(obj.obj + 0x1a0);
    return (checkVal != 0x1000000 || checkFloat != 450.0f);
}

bool ShouldSkipEntity(const DataStruct& obj) {
    // 这几类名字命中即直接跳过, 与幽灵判定无关(原本散落在渲染循环里, 现在收拢到一处)
    if (strstr(obj.class_name, "h55_joseph_camera") != NULL) return true;   // 约瑟夫相机
    if (strstr(obj.class_name, "redqueen_mirror") != NULL) return true;      // 红夫人镜子
    if (strstr(obj.class_name, "burke_console") != NULL) return true;        // 疯眼场景
    if (strstr(obj.class_name, "chr\\guajian") != NULL) return true;
    if (strstr(obj.class_name, "girl_e_sj_zuoyi") != NULL) return true;
    if (strstr(obj.class_name, "h55_survivor_w_shangren_tiaoban") != NULL) return true; // 商人跳板

    // 废弃模型(形态切换后留在数组里的旧形态)。这条取代了原来那份
    // "红蝶/无常/歌剧/破轮/木偶/冒险家" 的类名黑名单 —— 那份是按角色名猜的,
    // 漏一个角色就漏一个, 而且只在 inform_ghost 打开时才生效。
    // 现在直接问引擎: unit.another_model + 0x20 就是废弃形态的场景对象指针,
    // 精确、跟视角无关、不用维护名单。详见 PySelf.h 文件头。
    if (PySelf::is_stale_model(obj.obj)) return true;

    // 约瑟夫镜世界：镜像投影要专门显示成白色方框，它们的 +0x70(visible) 在对面世界常为 0，
    // 会被下面 IsGhostEntity 当幽灵在 inform_ghost 关时丢掉。这里显式放行(它们一定是本体集合里的玩家单位)。
    if (PySelf::body_is_mirror(obj.obj)) return false;

    bool is_ghost_obj = IsGhostEntity(obj);

    // 注意 +0x70 就是 NeoX 模型对象的 visible, 是**渲染剔除的结果**:
    // 废弃模型恒不可见, 但远处的真实对象同样不可见。所以它只能当"要不要按幽灵显示"的开关,
    // 绝不能当存在性/真假判据 —— 那会重演"求生者只看得到身边的密码机"。
    if (is_ghost_obj && !inform_ghost) return true;
    return false;
}

// ---- 签名候选(2026-10-30)：启动扫描不再"命中即锁"，bss 里所有矩阵签名、数组签名都记下来 ----
// 原来锁死的是一个槽位/一个数组：运行中该槽位被清空或挪作他用(用户实测"启动正常、运行中突然 3D 全没了，
// HUD 和 Python 层正常")就再也恢复不了，只能重开叠加层。现在锁的是签名，槽位/数组坏了就在候选里重选：
//   矩阵 —— 渲染线程每帧校验，失败当帧在签名窗口里重选(见 Draw_Main 的 find_matrix_slot)，锚点投影当真值
//   数组 —— 读线程每轮数本体集合(Python 侧 model+0x20)命中几个，一个没中就换命中最多的候选(try_switch_array)
// 候选全部失效才请求重扫 bss(读线程做，10 秒节流)。双缓冲：重扫写非活跃那份再翻转，渲染线程读活跃那份
static const int MAX_SIG = 32;   // 2026-11-04 数组新判据更宽，8 个怕把真的挤掉
struct SigSet { uint64_t msig[MAX_SIG]; int msig_n; long arr[MAX_SIG]; int arr_n; };
static SigSet g_sigs[2];
static std::atomic<int> g_sigs_active{0};
static std::atomic<bool> g_rescan_req{false};

// ===== 【临时诊断】方框偶发消失、骨骼还在(2026-11-05)。查完根因整段删(搜「临时诊断」) =====
// 渲染线程：骨骼画了、方框那段却没走到的本体，记成一个"缺框片段"，片段结束时打一行(持续帧数 + 开始那帧的原因)。
// 读线程：本体在快照里却没进 data[] 时打一行原因。
// g_scan_busy = 读线程此刻正在改写 data[]；g_scan_gen = 每发布一轮 +1。渲染帧期间两者有动静 = 可能撞上撕裂
#include <cstdarg>
static std::atomic<bool> g_scan_busy{false};
static std::atomic<uint32_t> g_scan_gen{0};
static std::atomic<int> g_diag_lines{0};  // 一次运行最多打 400 行，防刷屏
static void diag_log(const char *fmt, ...)
{
    if (g_diag_lines.fetch_add(1) >= 400) return;
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);                       // stdout 重定向到文件后是全缓冲，诊断行要立刻落盘
}

static void scan_signatures(const ModuleBssInfo &bss, SigSet &out)
{
    out.msig_n = out.arr_n = 0;
    long pages = (long)((bss.taddr - bss.addr) / 4096);
    long buff[512];
    for (long i = 0; i < pages; i++) {
        if (!vm_readv(bss.addr + i * 4096, buff, 0x1000)) continue;
        for (int ii = 0; ii < 512; ii++) {
            uint64_t here = bss.addr + i * 4096 + ii * 8;
            if (*(long long*)(&buff[ii]) == 0x656A624F72655028LL && out.msig_n < MAX_SIG)
                out.msig[out.msig_n++] = here;
            // 数组签名(以数组头槽位 H 为准)：H-0x40 低 32 位 = 257、H-0x48 = f32 1.0，H/H+8/H+0x10 是一组合法的
            // std::vector begin/end/cap。原来还要求 H-0x38 = 16384，2026-11-04 朋友机(新版 so)那里变成每次不同的值、
            // 老判据 0 命中；用户本机(0917 版)转储确认新判据同样成立，16384 那条已删
            if ((uint32_t)buff[ii] == 257 && out.arr_n < MAX_SIG && getFloat(here - 8) == 1.0f) {
                uint64_t H = here + 0x40;
                uint64_t b = getPtr64(H), e = getPtr64(H + 8), c = getPtr64(H + 0x10);
                bool vec_ok = b > 0x5000000000 && b < 0x8000000000 && e >= b && c >= e &&
                              ((e - b) & 7) == 0 && ((c - b) & 7) == 0 && (c - b) / 8 <= 10000;
                if (vec_ok) out.arr[out.arr_n++] = (long)(H - libbase);
            }
        }
    }
}

// ---- 数组签名兜底：用 Python 层反推场景数组(2026-11-04) ----
// 朋友机(另一版 so，矩阵签名 +0xAA5F338、槽位 +0x330)实测数组签名 16384/257/1.0f 一直 0 个命中，
// 扫描永远出不来。场景数组本质是一对 {begin, end} 指针，元素就是各对象的场景指针 —— 而 Python 层
// 本体集合(unit.model+0x20)给的正是"一定在这个数组里"的那几个指针。所以在 bss 里找每一对像
// {begin,end} 的相邻 qword，把它指向的数组读回来数本体命中，命中最多(平手取元素多的，场景总表是超集)
// 且至少命中一半本体的就是它。只有局内才有本体，大厅/准备阶段反推不了(那时等签名或等进局)
static ModuleBssInfo g_bss = {0, 0};

static long find_array_by_bodies()
{
    const uint64_t M = 0x00FFFFFFFFFFFFFFULL;
    uint64_t bodies[64];
    int nb = PySelf::copy_bodies(bodies, 64);
    if (nb <= 0 || g_bss.addr == 0 || g_bss.taddr <= g_bss.addr) return 0;
    std::sort(bodies, bodies + nb);
    const long pages = (long)((g_bss.taddr - g_bss.addr) / 4096);
    static uint64_t buff[513];
    static uint64_t elems[10000 + 512];       // 尾部留余量：朋友机的驱动见过"读内存超写"
    long best = 0; int best_hits = 0; uint64_t best_n = 0; int cand = 0;
    for (long i = 0; i < pages; i++) {
        uint64_t page = g_bss.addr + i * 4096;
        // 多读 8 字节，跨页的那一对 {begin,end} 也能看到；最后一页或读失败退回整页
        int lim = 511;
        if (i + 1 < pages && vm_readv(page, buff, 0x1008)) lim = 512;
        else if (!vm_readv(page, buff, 0x1000)) continue;
        for (int ii = 0; ii < lim; ii++) {
            uint64_t a = buff[ii] & M, e = buff[ii + 1] & M;
            if (a <= 0x5000000000 || a >= 0x8000000000 || e <= a || e >= 0x8000000000) continue;
            if ((e - a) & 7) continue;
            uint64_t n = (e - a) / 8;
            if (n < (uint64_t)nb || n > 10000) continue;
            cand++;
            if (!vm_readv(a, elems, n * 8)) continue;
            int hits = 0;
            for (uint64_t j = 0; j < n; j++)
                if (std::binary_search(bodies, bodies + nb, elems[j] & M)) hits++;
            if (hits > best_hits || (hits == best_hits && hits > 0 && n > best_n)) {
                best_hits = hits; best_n = n; best = (long)(page + ii * 8 - libbase);
            }
        }
    }
    printf("[数组反推] 本体 %d 个 候选 %d 个 最佳偏移=0x%lx 命中 %d 元素 %llu\n",
           nb, cand, best, best_hits, (unsigned long long)best_n);
    if (best_hits > 0 && best_hits * 2 >= nb) return best;
    return 0;
}

// 反推成功后把数组头槽位前后的内容打进日志，用来找回新版的数组签名。
// 老签名(以数组头槽位 H 为准)：H-0x38 = u64 16384，H-0x40 = u32 257，H-0x48 = f32 1.0。
// 每个 qword 同时按 u64 / 两个 u32 / 两个 f32 打印，哪个变了一眼可见
static void dump_array_neighborhood(long head_off)
{
    uint64_t q[20];                                    // H-0x80 .. H+0x18
    uint64_t base = libbase + head_off - 0x80;
    if (!vm_readv(base, q, sizeof(q))) { printf("[数组签名] 槽位附近读取失败\n"); return; }
    printf("[数组签名] 数组头槽位 H=libbase+0x%lx，老签名 H-0x38=16384 H-0x40=257 H-0x48=1.0f\n", head_off);
    for (int i = 0; i < 20; i++) {
        long rel = -0x80 + i * 8;
        uint32_t lo = (uint32_t)q[i], hi = (uint32_t)(q[i] >> 32);
        float flo, fhi; memcpy(&flo, &lo, 4); memcpy(&fhi, &hi, 4);
        printf("[数组签名] H%s0x%02lx: %016llx u32=%u,%u f32=%g,%g\n", rel < 0 ? "-" : "+", rel < 0 ? -rel : rel,
               (unsigned long long)q[i], lo, hi, flo, fhi);
    }
}

// 反推一次要把 bss 整段过一遍(约 3700 页 + 候选数组)，节流 10 秒
static long find_array_by_bodies_throttled()
{
    static time_t last = 0;
    time_t now = time(NULL);
    if (now - last < 10) return 0;
    last = now;
    return find_array_by_bodies();
}

// 当前数组一个本体都没命中时调：在其它候选里找命中最多的(至少 1 个)，找到就切过去。只在读线程里调
static bool try_switch_array()
{
    const SigSet &s = g_sigs[g_sigs_active.load()];
    static uint64_t elems[10000];
    long best = 0; int best_hits = 0;
    for (int k = 0; k < s.arr_n; k++) {
        if (s.arr[k] == ArrayaddrOffset) continue;
        uint64_t a = getPtr64(libbase + s.arr[k]), e = getPtr64(libbase + s.arr[k] + 8);
        if (a < 0x5000000000 || e <= a) continue;
        uint64_t n = (e - a) / 8;
        if (n == 0 || n > 10000) continue;
        if (!vm_readv(a, elems, n * 8)) continue;
        int hits = 0;
        for (uint64_t j = 0; j < n; j++)
            if (PySelf::is_body((uintptr_t)(elems[j] & 0x00FFFFFFFFFFFFFFULL))) hits++;
        if (hits > best_hits) { best_hits = hits; best = s.arr[k]; }
    }
    if (best_hits > 0) {
        printf("[数组切换] 0x%lx -> 0x%lx 本体命中 %d\n", ArrayaddrOffset, best, best_hits);
        ArrayaddrOffset = best;
        return true;
    }
    // 签名候选里也没有：拿本体集合在 bss 里反推(签名在新版本里失效时只能靠这条)
    long inferred = find_array_by_bodies_throttled();
    if (inferred != 0 && inferred != ArrayaddrOffset) {
        printf("[数组切换] 0x%lx -> 0x%lx (反推)\n", ArrayaddrOffset, inferred);
        ArrayaddrOffset = inferred;
        dump_array_neighborhood(inferred);
        return true;
    }
    g_rescan_req = true;    // 都没有：请求重扫 bss
    return false;
}

// ---- 场景数组按变化重扫(2026-10-30) ----
// 原来固定每 2 秒完整处理一遍，新召唤物/地上新掉的道具/准备阶段的预知最多晚 2 秒才出现。
// 现在每 100ms 把整个指针数组一次读回来(约 300 个指针 = 2.4KB，一次 vm_readv)，排序后跟上一轮比：
//   排序 = 只比"有哪些对象"，不管顺序(万一游戏会给数组重新排序，不至于每 100ms 都判成变化)
//   比内容而不是比头尾：删一个又加一个、中间删掉拿末尾补位时，个数和头尾都不变，只看头尾会漏
// 变了就立刻完整处理；两次完整处理至少隔 250ms(防止特效频繁生成销毁时扫个不停)；
// 不管变没变，满 2 秒保底处理一次(指针没变但对象换了模型、类名变了的情况，比指针是发现不了的)
static inline int64_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 读出整个指针数组(保持原顺序：data[] 的顺序有人依赖，见固定信息 C 的反向打印、预知的"最后一个监管")。
// 头、尾是两次独立读取，游戏恰好在中间扩容数组就会拿到新旧拼接的一对，再读一次头对不上就当无效
static bool read_array_snapshot(uint64_t &arr, std::vector<uint64_t> &out)
{
    uint64_t a = getPtr64(libbase + ArrayaddrOffset);
    uint64_t e = getPtr64(libbase + ArrayaddrOffset + 8);
    if (getPtr64(libbase + ArrayaddrOffset) != a) return false;
    if (a < 0x5000000000 || e <= a) return false;
    uint64_t n = (e - a) / 8;
    if (n == 0 || n > 10000) return false;
    out.resize(n);
    if (vm_readv(a, out.data(), n * 8)) {
        for (auto &p : out) p &= 0x00FFFFFFFFFFFFFFULL;
    } else {
        for (uint64_t i = 0; i < n; i++) out[i] = getPtr64(a + i * 8);   // 整块读失败(某页读不到)：逐个读，读不到的是 0
    }
    arr = a;
    return true;
}

// 完整处理完一轮后调：等到对象集合变了，或者满 2 秒
static void wait_array_change(const std::vector<uint64_t> &last_sorted)
{
    const int64_t start = mono_ms();
    std::vector<uint64_t> now;
    uint64_t a = 0;
    usleep(250 * 1000);
    while (mono_ms() - start < 2000) {
        if (!read_array_snapshot(a, now)) return;            // 读成无效(进局重建等)：交给主循环那条分支处理
        std::sort(now.begin(), now.end());
        if (now != last_sorted) return;
        usleep(100 * 1000);
    }
}

void read_thread(long int PD1,long int PD2,long int PD3)
{
    bool waitingLogged = false;
    while (pid <= 0) {
        pid = get_name_pid1("dwrg");
        if (pid > 0) break;
        if (!waitingLogged) {
            printf("[进程] 等待游戏进程启动\n");
            waitingLogged = true;
        }
        sleep(1);
    }
    driver->initialize(pid);
    printf("[进程] 已获取游戏进程 PID=%d 包名=%s\n", pid, extractedString);

    ModuleBssInfo result;
    // libbase 统一从maps取, 不用内核ioctl
    while (libbase == 0) {
        char mappath[64];
        snprintf(mappath, sizeof(mappath), "/proc/%d/maps", pid);
        FILE *fp = fopen(mappath, "r");
        if (fp == NULL) {
            printf("[进程] 无法打开 %s，重新等待游戏进程\n", mappath);
            pid = -1;
            while (pid <= 0) {
                sleep(1);
                pid = get_name_pid1("dwrg");
            }
            driver->initialize(pid);
            continue;
        }
        char line[1024];
        bool is_official = strstr(extractedString, "com.netease.idv") != NULL;
        while (fgets(line, sizeof(line), fp)){
            long a, t;
            if (sscanf(line, "%lx-%lx", &a, &t) != 2) continue;
            if (libbase == 0 && strstr(line, "r-xp")){
                // 国际服(Google Play)引擎打包在 split_config.arm64_v8a.apk 里直接 mmap,
                // 没有独立 libclient.so。按包名 com.netease.idv + 巨型段(>80MB)锁定引擎 r-xp,
                // 避开 webview(157MB,路径是 com.google.android.webview)和 split 里的小 so。
                // 该 r-xp 段起点就是 ELF 头(首个 LOAD 为 R E vaddr0,与国服同),libbase 直接取段起点。
                if (is_official && strstr(line, "com.netease.idv") && (t - a) > 0x5000000) libbase = a;
                if (!is_official && strstr(line, libso)) libbase = a;
            }
        }
        fclose(fp);
        if (libbase == 0) {
            printf("[基址] 暂未找到游戏模块，1秒后重试\n");
            sleep(1);
        }
    }
    if (strstr(extractedString, "com.netease.idv") != NULL)
        result = get_module_bssgjf(pid, ".");
    else
        result = get_module_bss(pid, libso);
    printf("[基址] libbase=0x%llX BSS=0x%llX-0x%llX\n",
           (unsigned long long)libbase, (unsigned long long)result.addr, (unsigned long long)result.taddr);
    g_bss = result;
    if (libbase < 0x5000000000){
        printf("[错误] libbase无效, 无法继续\n");
        sleep(9999);
    }

    // Py 模块和自动盖板只依赖 pid/libbase，不依赖场景签名，所以放在签名扫描之前启动(2026-11-04)。
    // 原来放在扫描循环之后：热更改掉数组签名时扫描永远出不来，Py 层跟着全灭(朋友机实测"Py根:未启动")。
    // 现在场景签名卡住时，面板 Py根/天赋/阶段/自身 这几行照样能显示，可以单独判断 Py 层在新版本下还能不能用
    // CPython 根(sys.modules + 类型对象)：读 so 的 ELF 段表定位 .PyRuntime，三个 Py 模块共用
    PyRoot::start(pid, libbase);
    // 密码机破译进度：走 CPython 对象图，自带后台线程(400ms 一轮)，跟这里的 3 秒大循环解耦
    PyProgress::start(libbase);
    // 天赋与辅助特质：同一条 CPython 链路，独立线程 500ms 一轮(一局内基本不变，不用刷那么勤)
    PyGenius::start(libbase);
    // 游戏阶段(大厅/准备/局内)：读 g_ui_mgr.ui_dict 里挂着哪个 UI 对象，独立线程 500ms 一轮
    PyScene::start(libbase);
    // 自身锚点(g_cam_ctrl.unit)与废弃模型黑名单：100ms 一轮，切换操控对象时要立刻跟上
    PySelf::start(libbase);
    // 林中童话(打水仗 BR)：玩家[1601]+物资[1602/1606]，独立线程 100ms 一轮，只在 waterfight_mode 时绘制
    PyWaterFight::start(libbase);
    // 道具白名单(cur_world.item_mgr._uid2item)：独立线程 100ms 一轮，只在「绘制道具」开着时读
    PyItems::start(libbase);
    // 蜡像师喷蜡辅助瞄准：独立线程 33ms 一轮，只在我是蜡像师时算(见 WaxAim.h)
    WaxAim::start(libbase);
    // 自动盖板：独立线程 10ms 一轮，板子列表由下面的读取循环每轮发布过去
    AutoPallet::start();

    while (MatrixOffset==0||ArrayaddrOffset==0)
    {
        // 一趟扫完 bss，所有签名都进候选(见 SigSet)，再从候选里挑初始的。挑错了运行中会自己换
        scan_signatures(result, g_sigs[0]);
        g_sigs_active = 0;
        const SigSet &s = g_sigs[0];
        printf("[签名] 矩阵签名 %d 个 数组签名 %d 个\n", s.msig_n, s.arr_n);
        // 2026-09-17 热更后 +0x430 恒为0。签名后是一组步长0x88的指针，三个元素都能走到
        // 同一个矩阵对象，这次更新只是让它整体位移了0x10。写死任何一个下次还会废，
        // 改成在窗口内找"整条链都走得通"的槽位(p1有效 且 p1+0xa58 也有效)，自愈。
        // 注意上界：只判 >0x5000000000 会让浮点垃圾(如0x400674954293DF)也蒙混过关，
        // 本机用户态指针都在 0x77xx/0x79xx 段，统一卡 <0x8000000000。
        for (int k = 0; k < s.msig_n && MatrixOffset == 0; k++){
            for (int off = 0x300; off <= 0x500; off += 8){
                uint64_t cand_slot = s.msig[k] + off;
                uint64_t p1 = getPtr64(cand_slot);            // getPtr64 已做 0xB4 掩码
                if (p1 <= 0x5000000000 || p1 >= 0x8000000000) continue;
                uint64_t p2 = getPtr64(p1 + 0xa58);
                if (p2 <= 0x5000000000 || p2 >= 0x8000000000) continue;
                MatrixOffset = cand_slot - libbase;
                printf("[矩阵命中] 签名=0x%llX 槽位=+0x%X 根=0x%llX MatrixOffset=0x%lx\n",
                       (unsigned long long)s.msig[k], off, (unsigned long long)cand_slot, MatrixOffset);
                break;
            }
        }
        // 候选可能不止一个：挑元素最多的(场景总表有两三百个对象，别的小数组比不过)。两台机实测都只有 1 个候选。
        // 挑错了进局后 try_switch_array 会按 Python 本体集合换
        if (ArrayaddrOffset == 0) {
            long pick = 0; uint64_t pick_n = 0;
            for (int k = 0; k < s.arr_n; k++){
                uint64_t tmpArr = getPtr64(libbase + s.arr[k]);
                uint64_t tmpEnd = getPtr64(libbase + s.arr[k] + 8);
                uint64_t n = (tmpArr > 0x5000000000 && tmpEnd > tmpArr) ? (tmpEnd - tmpArr) / 8 : 0;
                printf("[数组候选] 偏移=0x%lx 元素 %llu\n", s.arr[k], (unsigned long long)n);
                if (n == 0 || n > 10000) continue;
                if (n > pick_n) { pick_n = n; pick = s.arr[k]; }
            }
            if (pick != 0) {
                ArrayaddrOffset = pick;
                printf("[数组命中] 偏移=0x%llX 元素 %llu\n", (unsigned long long)ArrayaddrOffset, (unsigned long long)pick_n);
                dump_array_neighborhood(ArrayaddrOffset);   // 签名正常的机器也打一份，当对照
            }
        }
        // 签名没找到(或找到但数组头无效)：局内用 Python 本体集合反推。Py 模块已在扫描前启动，进局后本体集合就有了
        if (ArrayaddrOffset == 0 && PySelf::body_set_ready()) {
            long inferred = find_array_by_bodies();
            if (inferred != 0) {
                ArrayaddrOffset = inferred;
                printf("[数组命中] 偏移=0x%llX (Python 反推)\n", (unsigned long long)ArrayaddrOffset);
                dump_array_neighborhood(inferred);
            }
        }
        if (MatrixOffset!=0 && ArrayaddrOffset!=0) break;
        sleep(5);
    }
    read_state = 2;

    // 这里只打一行日志；Arrayaddr/Count 由下面主循环每轮按快照重新赋值
    Arrayaddr = getPtr64(libbase + ArrayaddrOffset);
    uint64_t ArrayEnd = getPtr64(libbase + ArrayaddrOffset + 8);
    printf("[数组] Arrayaddr=0x%llX End=0x%llX Count=%d\n", (unsigned long long)Arrayaddr, (unsigned long long)ArrayEnd, (int)((ArrayEnd - Arrayaddr) / 8));

    // 读空的轮次一律不发布：准备阶段 -> 进局那一下游戏会重建场景数组，偶尔一轮正好撞上：
    // 数组头读出来无效，或数组有效但一个对象都没收到。原来这两种情况都当场发布空列表，
    // 然后等 1~2 秒才扫下一轮 —— 表现就是"加载前明明都读到了，进局后全部消失两秒"。
    // 现在读空就保留上一轮(绘制侧照旧)，照常 2 秒后再扫。不需要"连续读空就清零"：
    // 每局都会重建数组，下一轮有效扫描会整体覆盖，不会有跨局残留。
    // 也不需要快速重试：对象地址一局内不变，上一轮可信就一直可信，重扫只是为了收新建的对象。
    // 两轮之间的等待见 wait_array_change(对象集合变了立刻扫，否则 2 秒保底)
    time_t last_rescan = 0;
    std::vector<uint64_t> snap, snap_sorted;
    while (true)
    {
        // 候选全部失效时(数组这边 try_switch_array、矩阵那边渲染线程)请求的重扫。10 秒节流，写非活跃那份再翻转
        if (g_rescan_req.load() && time(NULL) - last_rescan >= 10) {
            g_rescan_req = false;
            last_rescan = time(NULL);
            int w = 1 - g_sigs_active.load();
            scan_signatures(result, g_sigs[w]);
            g_sigs_active = w;
            printf("[签名重扫] 矩阵签名 %d 个 数组签名 %d 个\n", g_sigs[w].msig_n, g_sigs[w].arr_n);
        }
        // 整个指针数组一次读回来(头尾校验见 read_array_snapshot)，下面逐项处理用这份快照
    	uint64_t curArray = 0;
	    if (!read_array_snapshot(curArray, snap)) {
            read_state = 1;   // 只影响面板状态文字，不影响绘制(Draw_Main 只在 0 时跳过)
            // 局内(有本体集合)还读成无效：先看别的候选里有没有装着本体的，有就立刻换过去重扫。
            // 进局重建数组那一下所有候选都没有本体，不会误切
            if (PySelf::body_set_ready() && try_switch_array()) continue;
            sleep(2);
            continue;
        }
        Arrayaddr = curArray;
        Count = (long)snap.size();
        read_state = 2;
    	int entity_count=0;
        // 镜面这几个指针和预知文本先收在局部变量里，扫完一次性发布(见本轮末尾)。
        // 原来是"每轮先把全局清零、扫描中途再赋值"，绘制线程撞进这段(约1ms)就会读到 0，
        // 表现是红夫人模式下镜线约每分钟消失一帧。收在局部里，全局永远要么是上一轮的值、要么是新值。
        // 清零的本意(防跨局残留)仍然保留：局部变量每轮从 0 开始，发布时整体覆盖。
        uintptr_t rq_local = 0, rq_mirror_local = 0, mirror_local = 0, mirror_preview_local = 0;
        char prophet_local[sizeof(prophet_text)];
        prophet_local[0] = 0;
        // 自动盖板用的板子(类名含 woodplane)。getscene() 不认板子、它们进不了 data[]，所以单独收
        uint64_t boards_local[AutoPallet::MAX_BOARDS];
        int boards_n = 0;
        // 准备阶段的预知直接读房间里的监管 pid(ui_dict['UIRoomWait'].b_uid2pid)查表，不看场景模型(2026-10-26 反编译)：
        // 联合狩猎匹配局里我是求生者时，UIRoomWait._get_butcher_pos_id 把两个监管都分到座位 -1，
        // 后进房的那个 update_role 会 destroy_model 掉前一个 —— 场景里永远只剩一个对方监管的模型，按类名怎么猜都只有一个。
        // b_uid2pid 是 _show_butcher 在建模型之前给每个监管写的，模型就是用这个 pid 建的。
        // 读不到(或有 pid 读失败)时退回按场景类名拼名字。大厅残留的监管模型也因此不用再单独过滤
        const bool phase_lobby = PyScene::in_lobby();
        int room_hunters = -1;
        int room_pids[2] = {-1, -1};
        if (!phase_lobby && PyScene::in_prepare() && PyRoot::g_ready.load()) {
            const char *why = "";
            PyCore::read_room_hunter_count(room_hunters, why, room_pids);
        }
        g_room_hunter_n = room_hunters;
        bool room_pid_ok = room_hunters > 0;
        for (int k = 0; k < room_hunters && k < 2; k++) if (room_pids[k] < 0) room_pid_ok = false;
        std::string scene_last_hunter;   // room_pid_ok 时场景里最后一个监管的名字，pid 查不到表时兜底用
        // 真值：局内本体集合(Python 侧 unit.model+0x20)应该都在场景数组里。一个都没中 = 数组锁错/被挪作他用
        const bool truth_ready = PySelf::body_set_ready();
        int truth_hits = 0;
        // 【临时诊断】本体没进 data[] 的原因，按对象记，原因变了才打一行(0 = 进了 data[])
        static std::unordered_map<uint64_t, int> diag_reason;
        auto body_note = [&](uint64_t obj, int reason, int extra) {
            auto it = diag_reason.find(obj);
            int prev = it == diag_reason.end() ? 0 : it->second;
            if (prev == reason) return;
            static const char *const RN[] = {"已进data[]", "不在场景数组快照里", "类名长度无效", "类名内容读失败",
                                             "快照里重复", "类名不含player/boss", "分类分支continue"};
            diag_log("[框诊断] 读线程 本体0x%llx %s -> %s (extra=%d gen=%u)\n", (unsigned long long)obj,
                     RN[prev], RN[reason], extra, g_scan_gen.load());
            if (reason == 0) diag_reason.erase(obj); else diag_reason[obj] = reason;
            if (diag_reason.size() > 256) diag_reason.clear();
        };
        g_scan_busy = true;   // 【临时诊断】下面开始原地改写 data[]
        for (int ii = 0; ii < Count && entity_count < 1000; ii++){
            cur_obj = snap[ii];

    		if (cur_obj == 0)
        		continue;
    		const bool diag_body = truth_ready && PySelf::is_body(cur_obj);
    		if (diag_body) truth_hits++;
    		
    	    // 类名要走五跳指针再加长度和内容，共 7 次驱动读；一轮 300 个对象就是 2000 多次，
    	    // 实测占了整轮 12ms 里的一半。同一个对象活着的时候类名不会变，所以按地址缓存。
    	    // ⚠ 地址会被复用(对象释放后新对象可能落在同一地址)，所以缓存**必须带校验**：
    	    // 每轮仍读一次第一跳(obj+0xf8)，值对得上才用缓存。7 次读降到 1 次，又不会张冠李戴。
    	    // 校验不是绝对的(新对象的第一跳恰好相同就会漏网)，但那种情况下最多错一轮的标签，
    	    // 下一轮对象就稳定了；换来的是整轮耗时减半。
    	    //
    	    // ★ 读失败时沿用缓存(2026-09-23)：内存压力下类名那条链要碰约 6 个堆页，
    	    //   任何一个被换进 zram 就整条断掉、对象被丢弃 —— 这正是"实体数量整体塌陷"的成因。
    	    //   而校验只碰对象自己那一页。所以这里必须分清"读失败"和"值就是 0"：
    	    //   getPtr64 两种情况都返回 0(内部 T result{} 值初始化)，因此改用 vm_readv 拿返回值。
    	    //   读失败 -> 沿用缓存里的类名，让对象继续画出来(坐标仍是实时读的，位置不会错)；
    	    //   代价是万一该地址已换成别的对象，标签会是旧的 —— 比整个消失可接受。
    	    uint64_t chain_head = 0;
    	    bool head_ok = vm_readv(cur_obj + 0xf8, &chain_head, 8);
    	    chain_head &= 0x00FFFFFFFFFFFFFFULL;
    	    auto nc = g_name_cache.find(cur_obj);
    	    if (nc != g_name_cache.end() && (!head_ok || nc->second.first == chain_head)) {
    	        filter_class_name = nc->second.second;
    	    } else {
    	        uint64_t class_name_obj = getPtr64(getPtr64(getPtr64(getPtr64(chain_head)+0x8)+0x20)+0x20)+0x0;
    	        int len = getDword(class_name_obj + 0x10);
    	        if (len >= 256 || len == 0 || len < 0) {
    	            if (diag_body) body_note(cur_obj, 2, len);
    	            continue;
    	        }
    	        filter_class_name.resize(len);
    	        // 内容读不到时 resize 后的缓冲里留着上一个对象类名的前半截，不能当类名、更不能进缓存
    	        // (缓存只靠第一跳校验，写错一次就错一整局)。跳过本轮，下一轮再读
    	        if (!vm_readv(getPtr64(class_name_obj + 0x8), &filter_class_name[0], len)) {
    	            if (diag_body) body_note(cur_obj, 3, len);
    	            continue;
    	        }
    	        // 跨局会不断有新对象，攒太多就整体清一次，下一轮重建(只是多花一轮的读取)
    	        // 每条约 180 字节(map 节点 + 类名字符串)：对局中约 300 条≈50KB，1500 条≈250KB
    	        if (g_name_cache.size() > 1500) g_name_cache.clear();
    	        g_name_cache[cur_obj] = std::make_pair(chain_head, filter_class_name);
    	    }

    	    if (boards_n < AutoPallet::MAX_BOARDS && filter_class_name.find("woodplane") != std::string::npos) {
    	        bool seen = false;
    	        for (int k = 0; k < boards_n; k++) if (boards_local[k] == cur_obj) { seen = true; break; }
    	        if (!seen) boards_local[boards_n++] = cur_obj;
    	    }

			int is_dup=0;
			float pd1 = getFloat(cur_obj + 0x1a0);   // 原来还读一次 +0x298(pd2)，没人用，已删
			for (int i = 0; i < entity_count; i++){
        		if(cur_obj == data[i].obj){
        		    is_dup=1;
        		}        		    
        	}
        	if (is_dup == 1){
    		    continue;   // 已经在 data[] 里了(同一指针出现两次)，不算缺
    		}
        	// 过滤随从等无关对象。Python 层的角色本体(unit.model+0x20)不过黑名单：台球手球形态的模型是
        	// chr/boss/billy/billy_weapon01.gim(皮肤 14801，BilliardComponent.billiard_change_model 换上)，会被 "weapon" 误杀(2026-10-25)
        	if (should_filter(filter_class_name) && !PySelf::is_body(cur_obj)) {
        		continue;
        	}
        	std::string s;
        	//预知监管者
            // 局内：CPython 侧有监管单位，直接取它 unit.model 对应的场景对象的类名。
            //   这样另一形态(无常黑白)、约瑟夫相机、大厅残留都不会顶掉真监管。
            // 准备阶段：读得到房间监管 pid 就不看场景(见本轮开头 room_pid_ok)，本轮末尾按 pid 填。
            // 大厅/准备阶段读不到 pid：units_by_type 里还没有监管单位(实测)，退回下面按类名匹配的老逻辑。
            //   ⚠ 准备阶段不能用 +0x73(visible) 过滤：打求生者时真监管的模型在准备界面是不可见的(实测)
            // 联合狩猎有两个监管(2026-10-25)：两个名字都拼进预知(去重)，见 prophet_append
            uint64_t real_hunter = 0;
            const bool has_real_hunter = PySelf::hunter_body(real_hunter);
            if (show_draw_prophet && has_real_hunter){
                if (PySelf::is_butcher_body(cur_obj)) prophet_append(prophet_local, sizeof(prophet_local), getboss_prophet(filter_class_name.c_str()));
            }
            else if (show_draw_prophet){//预知开始
                if (strstr(filter_class_name.c_str(), "burke_console") == NULL&&strstr(filter_class_name.c_str(), "h55_joseph_camera") == NULL&&strstr(filter_class_name.c_str(), "redqueen_e_heijin_yizi") == NULL&&strstr(filter_class_name.c_str(), "_lod") == NULL){
                    if (strstr(filter_class_name.c_str(), "boss") != NULL){
                        if (room_pid_ok) scene_last_hunter = getboss_prophet(filter_class_name.c_str());   // 名字可能就是 filter_class_name 本身，必须拷贝
                        else prophet_append(prophet_local, sizeof(prophet_local), getboss_prophet(filter_class_name.c_str()));
                    }
                }
            }//预知结束
    		
			if (strstr(filter_class_name.c_str(), "player") != NULL||strstr(filter_class_name.c_str(), "boss") != NULL || pd1 == 450 || strstr(filter_class_name.c_str(), "scene") != NULL || strstr(filter_class_name.c_str(), "prop") != NULL || strstr(filter_class_name.c_str(), "mirror") != NULL || Debugging )
			{
    			data[entity_count].obj = cur_obj;
    			// 阵营/str 必须先清零：下面那串 if-else 只有五个分支，而入口条件里的
    			// `pd1 == 450` 和 `Debugging` 能让对象进来却一个分支都不命中。
    			// data[] 是全局数组、原地复用，不清就会**沿用上一帧同下标那个对象的
    			// 阵营和名字** —— 表现是装饰物被当成角色画出来、还顶着别人的名字，
    			// 并且 内核人物数量 虚高。正常游玩时类名基本都能命中 player/boss，
    			// 所以这个洞主要在开「绘制调试」时发作。
    			data[entity_count].camp = 0;
    			data[entity_count].str[0] = '\0';
    			// 鹿头夹子(ClaspTrapUnit，skill_creature 73)：模型放在监管目录下，按类名会归监管、再被本体集合挡掉，
    			// 本体集合不可用时还会被画成红色「[鹿头]」。这里先截下来归道具。预知监管上面已经算过，仍归鹿头
    			if (strstr(filter_class_name.c_str(), "dm65_butcher_ll") != NULL && strstr(filter_class_name.c_str(), "trap.gim") != NULL){
    			const char* prop_result = getprop(filter_class_name.c_str());
    			if (prop_result == NULL) { if (diag_body) body_note(cur_obj, 6, 0); continue; }
    			strcpy(data[entity_count].str, prop_result);
    			data[entity_count].camp=4;
    			}
    			else if (strstr(filter_class_name.c_str(), "boss") != NULL){
    			//data[指针数量].str=getboss(过滤类名.c_str());
    			strcpy(data[entity_count].str, getboss(filter_class_name.c_str()));
    			data[entity_count].camp=1;
    			}
    			else if (strstr(filter_class_name.c_str(), "player") != NULL){
    			//data[指针数量].str=getplayer(过滤类名.c_str());
    			strcpy(data[entity_count].str, getplayer(filter_class_name.c_str()));
    			data[entity_count].camp=2;
    			}
    			else if (strstr(filter_class_name.c_str(), "scene") != NULL){
    			const char* scene_result = getscene(filter_class_name.c_str());
    			if (scene_result == NULL) { if (diag_body) body_note(cur_obj, 6, 1); continue; }
    			strcpy(data[entity_count].str, scene_result);
    			data[entity_count].camp=3;
    			}
    			else if (strstr(filter_class_name.c_str(), "prop") != NULL){
    			const char* prop_result = getprop(filter_class_name.c_str());
    			if (prop_result == NULL) { if (diag_body) body_note(cur_obj, 6, 2); continue; }
    			strcpy(data[entity_count].str, prop_result);
    			data[entity_count].camp=4;
    			}

    			else if (strstr(filter_class_name.c_str(), "redqueen") != NULL&&strstr(filter_class_name.c_str(), "mirror") != NULL&&strstr(filter_class_name.c_str(), "model") != NULL){
    			// fx/model/redqueen_mirror_model_obj_001.gim：准备放镜时的预览镜子(PlaceIndicator.rtc_model)，
    			// 常驻复用同一个对象，只在准备阶段可见。用法见 Draw_Main 里的镜线计算
    			data[entity_count].camp=5;
    			mirror_preview_local = cur_obj;
    			}
    			//sprintf(data[指针数量].类名, "%s", 过滤类名.c_str());
    			strcpy(data[entity_count].class_name, filter_class_name.c_str());
    			data[entity_count].objcoor=getPtr64(cur_obj+0x28);
    			if (!first_frame_logged){
        			printf("[实体] %s 地址=0x%llX 坐标地址=0x%llX 坐标=(%.1f,%.1f,%.1f) 类名=%s\n",
        			       data[entity_count].str, (unsigned long long)cur_obj,
        			       (unsigned long long)data[entity_count].objcoor,
        			       getFloat(data[entity_count].objcoor + 0xa0),
        			       getFloat(data[entity_count].objcoor + 0xa4),
        			       getFloat(data[entity_count].objcoor + 0xa8),
        			       data[entity_count].class_name);
        		}
    			if (diag_body) body_note(cur_obj, 0, 0);
    			entity_count++;
			}
			else if (diag_body) body_note(cur_obj, 5, 0);
    			
			//红夫人模式：本体和镜中红夫人的类名都是 redqueen.gim，只能靠对象字段区分。
			// 2026-09-22 热更后旧判据失效：镜中红夫人的 +0x70 不再是 65150(鬼魂)，也在地面上，
			// 于是被当成本体，镜面算错，求生者镜像跟着红夫人跑。
			// 现在用 +0x6D 实体种类位(实测，镜子放出状态)：
			//   本体       0x50 = 0x40(角色实体) | 0x10
			//   镜中红夫人 0x90 = 0x80(不占玩家槽位的角色) | 0x10
			// 整字节会跳变(见过 0x50->0xD0)，但 0x40 位稳定，所以只看位不看整字节。
			// 镜面 = 本体与镜中红夫人连线的垂直平分线，已用 Python 侧 MaryMirrorUnit.position/direction 验证。
			// 镜子没放出时镜中红夫人停在 y≈-1000，下面坐标读取处的 Z>=-300 会让 mirror=false。
			if (pd1==450){
			    uintptr_t coorPtr = getPtr64(cur_obj + 0x28);
			    if (strstr(filter_class_name.c_str(), "boss") != NULL && strstr(filter_class_name.c_str(), "redqueen") != NULL && strstr(filter_class_name.c_str(), "mirror") == NULL
			        && getFloat(coorPtr + 0xa0) != 0 && getFloat(coorPtr + 0xa8) != 0) {
			        uint8_t kind = 0;
			        vm_readv(cur_obj + 0x6D, &kind, 1);
			        if (kind & 0x40)      rq_local = cur_obj;       // 本体
			        else if (kind & 0x80) rq_mirror_local = cur_obj;   // 镜中红夫人
			    }
    			if (strstr(filter_class_name.c_str(), "boss") != NULL && strstr(filter_class_name.c_str(), "mirror") != NULL
    			    && getFloat(coorPtr + 0xa0) != 0 && getFloat(coorPtr + 0xa8) != 0)
    			{
    		    	mirror_local=cur_obj;
    			}    			
			}    						
        }
        g_scan_busy = false;   // 【临时诊断】data[] 改写结束
        // 【临时诊断】本体集合里、却根本不在这份快照里的
        if (truth_ready) {
            uint64_t bodies[64];
            int nb = PySelf::copy_bodies(bodies, 64);
            for (int k = 0; k < nb; k++) {
                bool found = false;
                for (uint64_t p : snap) if (p == bodies[k]) { found = true; break; }
                if (!found) body_note(bodies[k], 1, (int)snap.size());
            }
            if (truth_hits == 0)
                diag_log("[框诊断] 读线程 本轮本体命中 0 (快照 %d 项, 本体 %d 个)\n", (int)snap.size(), nb);
        }
        // 本轮一个对象都没收到：不发布，保留上一轮，按正常周期等下一轮(见循环前的说明)
        // 本体一个都不在这个数组里：换到装着本体的候选立刻重扫，这一轮不发布(绘制侧沿用上一轮)
        if (truth_ready && truth_hits == 0 && try_switch_array()) continue;
        snap_sorted = snap;
        std::sort(snap_sorted.begin(), snap_sorted.end());
        if (entity_count == 0) {
            wait_array_change(snap_sorted);
            continue;
        }
        if (!first_frame_logged){
            first_frame_logged = true;
            printf("[首帧调试] 矩阵16值已打印 实体列表已打印\n");
        }
        // 本轮结果一次性发布。prophet_local 为空(这轮没扫到监管)时保留上一轮的文本，
        // 跟原来"prophet_text 从不清零"的行为一致
        redqueen_obj        = rq_local;
        redqueen_mirror_obj = rq_mirror_local;
        mirror_obj          = mirror_local;
        mirror_preview_obj  = mirror_preview_local;
        if (show_draw_prophet && room_pid_ok) {
            int unknown_pids[2], n_unknown = 0;
            for (int k = 0; k < room_hunters && k < 2; k++) {
                const char *nm = getboss_by_pid(room_pids[k]);
                if (nm) prophet_append(prophet_local, sizeof(prophet_local), nm);
                else unknown_pids[n_unknown++] = room_pids[k];
            }
            // 表里没有的 pid(配表之后新出的监管)退回旧逻辑：准备阶段场景里最后一个监管模型。
            // 大厅名单之前一直是这样单个覆盖，实测胜出的是准备房里的监管、不会报大厅残留(Name.h 也没有时显示原始类名)。
            // 场景也给不出新名字时显示 pid，方便补表
            if (n_unknown > 0) {
                size_t before = strlen(prophet_local);
                if (!scene_last_hunter.empty()) prophet_append(prophet_local, sizeof(prophet_local), scene_last_hunter.c_str());
                if (strlen(prophet_local) == before) {
                    for (int k = 0; k < n_unknown; k++) {
                        char unknown[32];
                        snprintf(unknown, sizeof(unknown), g_english ? "[pid %d]" : "[监管pid%d]", unknown_pids[k]);
                        prophet_append(prophet_local, sizeof(prophet_local), unknown);
                    }
                }
            }
        }
        if (prophet_local[0]) memcpy(prophet_text, prophet_local, sizeof(prophet_text));
        AutoPallet::publish_boards(boards_local, boards_n);
        data_count = entity_count;
        g_scan_gen++;          // 【临时诊断】发布了一轮
        // 2026-09-23：3 秒 -> 2 秒(一轮实测约 9ms)。2026-10-30：改成对象集合变了就扫(最快 250ms)，2 秒保底
        wait_array_change(snap_sorted);
    }
}






// ---- 场景对象"是否本局真实存在"判定 ----
// 游戏把所有候选刷新点、以及角色的每种形态都实例化成对象放进数组，本局/当前只激活其中一部分。
// 未激活的那些类名、坐标全都正常，光看类名/坐标区分不出来。
//
// **判据是 +0x70（代码里原本叫 jxpd/checkVal）**，它是三态的：
//     0          = 本局根本不存在 / 当前不是活跃形态
//     0x1000000  = 正常存在
//     其它非0    = 存在，但处于鬼魂/特殊状态（旧记录里的 65150 属于这一档）
//
// 怎么确认的：红蝶本体与 opposite 形态做变身前后差分，两个对象在 0x300 字节里
// **各自只有 +0x70 这一列发生变化，且方向相反**（本体 0x1000000->0，opposite 0->0x1000000），
// 同时对照组(9个约瑟夫相机 + 15个密码机)零变化。语义非常干净。
// 横向验证：约瑟夫相机(每局默认加载但不存在) 4/4 全为 0；求生者 8 个对象里 +0x70!=0 的正好 4 个(实际人数)。
//
// 注意 +0x1a0 是"类型"字段(450=角色 500=可交互物)，**不是存在性**：
// 曾经误用它单独做判据，结果一个不存在的密码机 +0x1a0 恰好就是 500，照样被画出来。
//
// 曾经用过 +0x30(实例化节点指针)，**已废弃**：它不稳定，同样是约瑟夫相机，
// 一局里测到 13个只有1个非空，另一局 9个全部非空，不能用。
// +0x6D 与 +0x73 都是单字节，分别藏在 +0x6C / +0x70 这两个 dword 里。
//   +0x73 = 1  -> 当前活跃/在场（常见的 0x1000000 就是这个字节的 dword 形式）
//   +0x6D      -> 实体种类位域: 0x40=角色/生物  0x10=玩家阵营相关  0x00=纯场景装饰
static inline unsigned entity_active_byte(uintptr_t obj) { return (getDword(obj + 0x70) >> 24) & 0xFF; }
static inline unsigned entity_kind_byte(uintptr_t obj) { return (getDword(obj + 0x6C) >> 8)  & 0xFF; }

// **千万不要拿活跃位当"本局是否存在"用**：它是"当前对本机客户端可见"的意思，跟视角走。
// 实测同一张图，监管视角下 12 台密码机活跃位全是 1，换成求生者视角只剩 1 台是 1。
// 早先用它做密码机判据，导致求生者只能看到身边那一台，绕了一大圈才发现。
//
// 真正视角无关的存在性标记是 +0x240：
//   实测 约瑟夫相机 7/7 = 0，破轮台 4/4 = 0（这两类都是每局默认加载、本局并不存在的装饰）
//        密码机 真的 7 个 = 2 / 假的 5 个 = 0，箱子、求生者、监管 全部 = 2
static inline bool entity_exists(uintptr_t obj)
{
    return getDword(obj + 0x240) == 2;
}
// 破译进度配色：<20 绿、20~60 黄、>60 红
static inline ImColor progress_color(float v)
{
    if (v < 20.0f)  return color_green;
    if (v <= 60.0f) return color_yellow;
    return color_red;
}

// 人物方框：只画四个边角(每边中间留空)，减少遮挡。参数顺序跟 AddRect 一致方便替换，rounding/flags 不用。
// 边角臂长取短边的 30%(方框宽 = 高/2，所以横竖臂等长)
static void corner_box(ImVec2 a, ImVec2 b, ImU32 col, float /*rounding*/ = 0.f, int /*flags*/ = 0, float thick = 1.8f)
{
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float L = fminf(fabsf(b.x - a.x), fabsf(b.y - a.y)) * 0.3f;
    ImVec2 c[4][3] = {
        {{a.x, a.y + L}, {a.x, a.y}, {a.x + L, a.y}},   // 左上
        {{b.x - L, a.y}, {b.x, a.y}, {b.x, a.y + L}},   // 右上
        {{b.x, b.y - L}, {b.x, b.y}, {b.x - L, b.y}},   // 右下
        {{a.x + L, b.y}, {a.x, b.y}, {a.x, b.y - L}},   // 左下
    };
    for (auto &p : c) dl->AddPolyline(p, 3, col, 0, thick);
}

// 骨骼包围盒(屏幕坐标)：骨骼轮每帧按单位(M = 场景对象)累计，方框绘制时取用。一帧最多 32 个单位
static uint64_t g_bone_box_m[32];
static float    g_bone_box[32][4];      // x1, y1, x2, y2
static int      g_bone_box_n = 0;
static int bone_box_slot(uint64_t M, bool create)
{
    for (int k = 0; k < g_bone_box_n; k++) if (g_bone_box_m[k] == M) return k;
    if (!create || g_bone_box_n >= 32) return -1;
    int k = g_bone_box_n++;
    g_bone_box_m[k] = M;
    g_bone_box[k][0] = g_bone_box[k][1] = 1e30f;
    g_bone_box[k][2] = g_bone_box[k][3] = -1e30f;
    return k;
}
static void bone_box_add_point(uint64_t M, ImVec2 p)
{
    int k = bone_box_slot(M, true);
    if (k < 0) return;
    if (p.x < g_bone_box[k][0]) g_bone_box[k][0] = p.x;
    if (p.y < g_bone_box[k][1]) g_bone_box[k][1] = p.y;
    if (p.x > g_bone_box[k][2]) g_bone_box[k][2] = p.x;
    if (p.y > g_bone_box[k][3]) g_bone_box[k][3] = p.y;
}
static void bone_box_add_circle(uint64_t M, ImVec2 c, float r)
{
    bone_box_add_point(M, ImVec2(c.x - r, c.y - r));
    bone_box_add_point(M, ImVec2(c.x + r, c.y + r));
}
// 取包围盒并外扩一圈(短边的 10%，至少 4px)，盒太小(少于 2 根骨骼投影出来)算没有
static bool bone_box_get(uint64_t M, float &x1, float &y1, float &x2, float &y2)
{
    int k = bone_box_slot(M, false);
    if (k < 0) return false;
    float w = g_bone_box[k][2] - g_bone_box[k][0], h = g_bone_box[k][3] - g_bone_box[k][1];
    if (!(w > 1.f && h > 1.f)) return false;
    float pad = fmaxf(4.f, fminf(w, h) * 0.1f);
    x1 = g_bone_box[k][0] - pad; y1 = g_bone_box[k][1] - pad;
    x2 = g_bone_box[k][2] + pad; y2 = g_bone_box[k][3] + pad;
    return true;
}

// 一副骨架投到屏幕上的包围盒(含头圆，跟骨骼轮的画法一致)，外扩同 bone_box_get。reflect=true 时每根骨骼的水平两轴先关于镜线反射。
// 给不进骨骼轮的对象用(镜中红夫人、红夫人镜子里的求生者)：它们的方框也按骨骼来，跟普通人物一致
static bool skeleton_screen_box(const NativeBones::Skeleton &sk, bool reflect, float &x1, float &y1, float &x2, float &y2)
{
    ImVec2 bp[NativeBones::NUM_TARGET];
    bool bok[NativeBones::NUM_TARGET];
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    auto add = [&](float x, float y) { mnx = fminf(mnx, x); mny = fminf(mny, y); mxx = fmaxf(mxx, x); mxy = fmaxf(mxy, y); };
    int n = 0;
    for (int t = 0; t < NativeBones::NUM_TARGET; t++) {
        bok[t] = false;
        if (!sk.ok[t]) continue;
        float ox = sk.x[t], oy = sk.y[t], oz = sk.z[t];
        if (reflect) calculate_line_reflection(mirror_line_x1, mirror_line_y1, mirror_line_x2, mirror_line_y2, sk.x[t], sk.z[t], &ox, &oz);
        float cam = matrix[3]*ox + matrix[7]*oy + matrix[11]*oz + matrix[15];
        if (cam <= 0.01f) continue;
        bp[t] = ImVec2(px + (matrix[0]*ox + matrix[4]*oy + matrix[8]*oz + matrix[12]) / cam * px,
                       py - (matrix[1]*ox + matrix[5]*oy + matrix[9]*oz + matrix[13]) / cam * py);
        bok[t] = true; n++;
        add(bp[t].x, bp[t].y);
    }
    if (n < 2) return false;
    if (bok[0]) {                                     // 头圆：同骨骼轮(沿 脊椎->颈 往上顶)
        ImVec2 up(0.f, -1.f);
        float r = 6.0f;
        if (bok[1] && bok[2]) {
            float dx = bp[1].x - bp[2].x, dy = bp[1].y - bp[2].y, L = sqrtf(dx*dx + dy*dy);
            if (L > 0.5f) { up.x = dx / L; up.y = dy / L; r = L * 0.28f; }
        }
        r = fminf(fmaxf(r, 3.0f), 26.0f);
        ImVec2 base = bok[1] ? bp[1] : bp[0];
        ImVec2 hc(base.x + up.x * (r + 3.0f), base.y + up.y * (r + 3.0f));
        add(hc.x - r, hc.y - r); add(hc.x + r, hc.y + r);
    }
    float w = mxx - mnx, h = mxy - mny;
    if (!(w > 1.f && h > 1.f)) return false;
    float pad = fmaxf(4.f, fminf(w, h) * 0.1f);
    x1 = mnx - pad; y1 = mny - pad; x2 = mxx + pad; y2 = mxy + pad;
    return true;
}

// 【临时诊断】本帧画了骨骼的本体 / 本帧方框那段走到的对象(诊断说明见 diag_log 那段)
static uint64_t g_diag_bone[32];
static int      g_diag_bone_n = 0;
static uint64_t g_diag_box[64];
static int      g_diag_box_n = 0;

// 固定信息用：按指定像素字号画文字 / 量文字宽高(默认字号由字体决定，这里显式传 size 才能跟随滑动条)
static inline void fi_text(ImDrawList *dl, float x, float y, float size, ImU32 col, const char *txt)
{
    dl->AddText(ImGui::GetFont(), size, ImVec2(x, y), col, txt);
}
static inline ImVec2 fi_calc(float size, const char *txt)
{
    return ImGui::GetFont()->CalcTextSizeA(size, 99999.0f, 0.0f, txt);
}

// 画监管天赋树(径向图)。center/scale 给几何，gi.genius_levels[1..40] 给亮灭+等级。
static void DrawGeniusTree(ImDrawList *dl, ImVec2 center, float scale, const PyGenius::Info &gi)
{
    GeniusTree::ensure();
    auto POS = [&](int id) { return ImVec2(center.x + GeniusTree::g_px[id] * scale,
                                           center.y + GeniusTree::g_py[id] * scale); };
    ImFont *font = ImGui::GetFont();
    // 连线：只画两端都点亮的(未点节点整个不画，所以未点的连线也不画)
    for (int id = 1; id < GeniusTree::N; id++) {
        int p = GeniusTree::PARENT[id];
        if (!p) continue;
        if (!(gi.genius_levels[id] && gi.genius_levels[p])) continue;
        dl->AddLine(POS(p), POS(id), IM_COL32(255,190,60,255), scale*0.07f+1.0f);
    }
    // 中心枢纽
    dl->AddCircleFilled(center, scale * 0.26f, IM_COL32(150,80,110,255), 28);
    dl->AddCircle(center, scale * 0.26f, IM_COL32(220,170,200,255), 28, 2.0f);
    // 节点：只画已点的
    float rad = scale * 0.34f;
    float name_sz = rad * 0.95f; if (name_sz < 10) name_sz = 10;
    float lv_sz   = rad * 0.85f; if (lv_sz   < 11) lv_sz   = 11;
    for (int id = 1; id < GeniusTree::N; id++) {
        int lv = gi.genius_levels[id];
        if (!lv) continue;                                   // 没点的完全不显示
        ImVec2 p = POS(id);
        bool term = GeniusTree::is_terminal(id);
        ImU32 fill = term ? IM_COL32(255,195,30,255) : IM_COL32(255,130,35,255);
        ImU32 ring = term ? IM_COL32(255,240,120,255) : IM_COL32(255,230,170,255);
        dl->AddCircleFilled(p, rad, fill, 24);
        dl->AddCircle(p, rad, ring, 24, term ? scale*0.05f+2.0f : 2.0f);
        // 名称前 2 字(全中文，每字 3 字节 → 取前 6 字节不切断)/英文缩写，亮底黑字
        char head[7] = {0};
        const char *nm = g_english ? BUTCHER_GENIUS_ABBR_EN[id] : BUTCHER_GENIUS_NAME[id];
        int blen = 0; while (nm[blen] && blen < 6) blen++;
        memcpy(head, nm, blen);
        ImVec2 ts = font->CalcTextSizeA(name_sz, 99999.0f, 0.0f, head);
        dl->AddText(font, name_sz, ImVec2(p.x - ts.x/2, p.y - ts.y/2), IM_COL32(25,20,10,255), head);
        char ls[12]; snprintf(ls, sizeof(ls), "%d/%d", lv, BUTCHER_GENIUS_MAXLV[id]);
        ImVec2 lss = font->CalcTextSizeA(lv_sz, 99999.0f, 0.0f, ls);
        ImVec2 lp(p.x - lss.x/2, p.y + rad + 1.0f);
        dl->AddText(font, lv_sz, ImVec2(lp.x+1, lp.y+1), IM_COL32(0,0,0,220), ls);   // 黑描边
        dl->AddText(font, lv_sz, lp, IM_COL32(255,245,80,255), ls);
    }
}

static inline bool is_real_generator(uintptr_t obj)
{
    // 活跃位 + 一个密码机专用的辅助值。单用任何一个都不够：
    //   只判 +0x1a0 -> 未激活的候选机器该值恰好也可能是 500
    //   只判活跃位  -> 会混进一个位于原点(0,0,0)的占位对象
    return getFloat(obj + 0x1a0) == 500.0f && getDword(obj + 0x240) == 2;
}

// ---- VP 矩阵校验(指针范围 + 16 个值全部有限) ----
// 本机用户态指针都在 0x77xx/0x79xx 段，与启动扫描用的范围一致
static inline bool is_user_ptr(uint64_t p) { return p > 0x5000000000 && p < 0x8000000000; }
static inline bool matrix_finite(const float *m)
{
    for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return false;
    return true;
}

// 从槽位里读出的根指针 p1 走到矩阵：p1 -> +0xa58 -> 矩阵对象 -> +0x2c0。
// 除了指针范围和 16 个数有限，还要求是透视矩阵(w 那一列不全为 0)：正交的(阴影相机之类)投影出来没有近大远小，不可能是主相机
static bool read_matrix_root(uint64_t p1, uint64_t &mobj, float out[16])
{
    p1 &= 0x00FFFFFFFFFFFFFFULL;
    if (!is_user_ptr(p1)) return false;
    mobj = getPtr64(p1 + 0xa58);
    if (!is_user_ptr(mobj)) return false;
    if (!vm_readv(mobj + 0x2c0, out, 64) || !matrix_finite(out)) return false;
    return fabsf(out[3]) + fabsf(out[7]) + fabsf(out[11]) > 1e-6f;
}

// 真值：锚点(g_cam_ctrl.unit = 相机跟随的单位)用这个矩阵投影，必须在相机前方、落在屏幕附近(NDC ±1.5，放宽到屏外一点)
static bool anchor_in_view(const float *m, const float *a)
{
    float w = m[3]*a[0] + m[7]*a[1] + m[11]*a[2] + m[15];
    if (w <= 0.01f) return false;
    float nx = (m[0]*a[0] + m[4]*a[1] + m[8]*a[2] + m[12]) / w;
    float ny = (m[1]*a[0] + m[5]*a[1] + m[9]*a[2] + m[13]) / w;
    return fabsf(nx) <= 1.5f && fabsf(ny) <= 1.5f;
}

// 当前槽位不行时在所有矩阵签名的窗口里重选(跳过 skip)。每个签名的窗口一次读完，只有像指针的才往下走。
// 返回 2 = 找到通过真值的(没有真值时等于结构通过)；1 = 只找到结构通过、真值不过的(取第一个)；0 = 一个都读不出来
static int find_matrix_slot(uint64_t skip, const float *anc, uint64_t &slot_out, uint64_t &mobj_out, float out[16])
{
    const SigSet &s = g_sigs[g_sigs_active.load()];
    int got = 0;
    uint64_t win[65];
    float tmp[16];
    uint64_t mobj = 0;
    for (int k = 0; k < s.msig_n; k++) {
        if (!vm_readv(s.msig[k] + 0x300, win, sizeof(win))) continue;
        for (int j = 0; j < 65; j++) {
            uint64_t slot = s.msig[k] + 0x300 + j * 8;
            if (slot == skip) continue;
            if (!read_matrix_root(win[j], mobj, tmp)) continue;
            if (!anc || anchor_in_view(tmp, anc)) {
                slot_out = slot; mobj_out = mobj; memcpy(out, tmp, 64);
                return 2;
            }
            if (got == 0) { got = 1; slot_out = slot; mobj_out = mobj; memcpy(out, tmp, 64); }
        }
    }
    return got;
}
// 校验不过时沿用上一帧的矩阵，连续超过这么多帧才停止投影(约 80ms，肉眼看不出偏移)。
// 初值大于上限：启动后还没拿到过一次有效矩阵时，matrix[] 全是 0，不能拿来投影。
static constexpr int MATRIX_STALE_MAX = 5;
static int g_matrix_stale = MATRIX_STALE_MAX + 1;

// 模仿者身份的配色：侦探团蓝 / 模仿者红 / 中立黄 / 还没读到白。绘制和界面的身份簿列表共用
static ImColor copycat_color(int identity)
{
    switch (PySelf::copycat_camp(identity)) {
        case 1:  return ImColor(60,150,255,255);
        case 2:  return ImColor(255,60,60,255);
        case 3:  return ImColor(255,220,0,255);
        default: return ImColor(255,255,255,255);
    }
}

// 模仿者的名字："N号 身份"。身份进局约 2 秒后才赋值，之前只有号码(画白色)
static void copycat_label(int identity, int seat, char *out, size_t cap)
{
    const char *nm = PySelf::copycat_identity_name(identity);
    int len = seat > 0 ? snprintf(out, cap, g_english ? "#%d " : "%d号 ", seat) : 0;
    if (len < 0 || (size_t)len >= cap) len = 0;
    if (nm) snprintf(out + len, cap - len, "%s", nm);
    else if (identity) snprintf(out + len, cap - len, g_english ? "id%d" : "身份%d", identity);
    else snprintf(out + len, cap - len, "?");
}

// 林中童话配色按 water_fight_team(1~8，"八个阵营两两组队"指的就是它)：同队两人同色；0=未读到 白
static ImColor waterfight_color(int team)
{
    switch (team) {
        case 1:  return ImColor(240, 50, 50,255);   // 红
        case 2:  return ImColor(255,140,  0,255);   // 橙
        case 3:  return ImColor(255,225,  0,255);   // 黄
        case 4:  return ImColor( 70,210, 70,255);   // 绿
        case 5:  return ImColor(  0,220,210,255);   // 青
        case 6:  return ImColor( 60,150,255,255);   // 蓝
        case 7:  return ImColor(190,110,245,255);   // 紫
        case 8:  return ImColor(255, 80,190,255);   // 品红
        default: return ImColor(255,255,255,255);   // 未读到
    }
}
// label 用 water_fight_identity 动物名(个人，不是队)：1马鹿…10野猪
static const char *waterfight_name(int identity)
{
    static const char *N[11] = {"?","马鹿","野猫","狐狸","獾","雪兔","金雕","松鼠","草蛇","蝙蝠","野猪"};
    // 英文：国际服文本里只查到 Fox/Squirrel/Bat/Wild Boar，其余按常用英文名
    static const char *E[11] = {"?","Red Deer","Wildcat","Fox","Badger","Snow Hare","Golden Eagle","Squirrel","Grass Snake","Bat","Wild Boar"};
    return (identity >= 1 && identity <= 10) ? (g_english ? E[identity] : N[identity]) : "?";
}

// 这件道具是不是我的：主人是我(当前操控单位或主角色，机械师操控玩偶时两者不同)，
// 或者它在玩具商的包里、包贴在我身上(包里的道具 owner=None，模型每 tick 被挪到我脚下)
static bool item_is_mine(const PyItems::Entry &it, uint64_t anchor_obj)
{
    const int64_t me = PySelf::self_uid(), main = PyItems::main_uid();
    auto is_me = [&](int64_t u) { return u > 0 && (u == me || u == main); };
    if (it.owned) return is_me(it.owner_uid) || (anchor_obj != 0 && it.owner_body == anchor_obj);
    return is_me(it.attach_uid);
}

// ---- 蜡像师喷蜡辅助瞄准(WaxAim.h 算，这里只投影和画) ----
// 当前视线：屏幕中心对应的方向 d 满足 clip_x = clip_y = 0，即 d ⟂ (m0,m4,m8) 且 d ⟂ (m1,m5,m9)，取叉积再按 w>0 定号。
// 世界分量顺序和投影公式一致：(X, 高度, Y)
static bool wax_view_dir(float &dx, float &dh, float &dy)
{
    float a[3] = {matrix[0], matrix[4], matrix[8]}, b[3] = {matrix[1], matrix[5], matrix[9]};
    dx = a[1] * b[2] - a[2] * b[1]; dh = a[2] * b[0] - a[0] * b[2]; dy = a[0] * b[1] - a[1] * b[0];
    float n = sqrtf(dx * dx + dh * dh + dy * dy);
    if (!(n > 1e-9f)) return false;
    dx /= n; dh /= n; dy /= n;
    if (matrix[3] * dx + matrix[7] * dh + matrix[11] * dy < 0) { dx = -dx; dh = -dh; dy = -dy; }
    return true;
}

// 无穷远方向投影(w 不含平移)。天空中的瞄准线用它：相机转到该方向时它正好落在屏幕中心
static bool wax_proj_dir(float dx, float dh, float dy, ImVec2 &o)
{
    float w = matrix[3] * dx + matrix[7] * dh + matrix[11] * dy;
    if (w <= 1e-4f) return false;
    o.x = px + (matrix[0] * dx + matrix[4] * dh + matrix[8] * dy) / w * px;
    o.y = py - (matrix[1] * dx + matrix[5] * dh + matrix[9] * dy) / w * py;
    return true;
}

static bool wax_proj_pt(const float *p, ImVec2 &o)
{
    float w = matrix[3] * p[0] + matrix[7] * p[1] + matrix[11] * p[2] + matrix[15];
    if (w <= 0.01f) return false;
    o.x = px + (matrix[0] * p[0] + matrix[4] * p[1] + matrix[8] * p[2] + matrix[12]) / w * px;
    o.y = py - (matrix[1] * p[0] + matrix[5] * p[1] + matrix[9] * p[2] + matrix[13]) / w * py;
    return true;
}

static void draw_wax_aim(ImDrawList *Draw)
{
    float vx, vh, vy;
    if (!wax_view_dir(vx, vh, vy)) return;
    float hl = sqrtf(vx * vx + vy * vy);
    float elev_now = atan2f(vh, hl) * 180.f / (float)M_PI;
    if (hl > 1e-4f) WaxAim::set_view(vx / hl, vy / hl, elev_now);

    const WaxAim::Result &r = WaxAim::result();
    if (!WaxAim::enabled || !r.is_wax) return;
    const float mx = WaxAim::aim_x >= 0 ? WaxAim::aim_x : px, my = WaxAim::aim_y >= 0 ? WaxAim::aim_y : py * 2.f * WAX_MARK_Y;
    const ImVec2 off(mx - px, my - py);                    // 游戏瞄准标记相对屏幕中心的偏移
    const float D2R = (float)M_PI / 180.f;

    // 天空瞄准线：相机俯仰到 p 时视线仰角 = 现在的仰角 + (last_pitch - p)(last_pitch 正 = 往下看，2026-11-04 上机确认)
    // 当前俯仰正好在区间边界时 e = elev_now，线端点必然落在标记上 —— 所以线和标记对不对得上只取决于标记像素位置准不准
    auto sky = [&](float hx, float hz, float yaw, float p, ImVec2 &o) {
        float e = (elev_now + (r.last_pitch - p)) * D2R;
        float c = cosf(yaw), s = sinf(yaw);
        float ux = hx * c - hz * s, uz = hx * s + hz * c;
        if (!wax_proj_dir(cosf(e) * ux, sinf(e), cosf(e) * uz, o)) return false;
        o.x += off.x; o.y += off.y;
        return true;
    };
    for (int i = 0; i < r.nb; i++) {
        const WaxAim::Band &b = r.b[i];
        ImVec2 a0, a1, w0, w1;
        float pm = (b.p0 + b.p1) * 0.5f;
        if (!sky(b.hx, b.hz, 0, b.p0, a0) || !sky(b.hx, b.hz, 0, b.p1, a1)) continue;
        if (!sky(b.hx, b.hz, -b.half_yaw, pm, w0) || !sky(b.hx, b.hz, b.half_yaw, pm, w1)) continue;
        float th = sqrtf((w1.x - w0.x) * (w1.x - w0.x) + (w1.y - w0.y) * (w1.y - w0.y));
        th = th < 12.f ? 12.f : (th > 80.f ? 80.f : th);
        float len = sqrtf((a1.x - a0.x) * (a1.x - a0.x) + (a1.y - a0.y) * (a1.y - a0.y));
        if (len < 6.f) { float k = (6.f - len) * 0.5f / (len > 0.1f ? len : 1.f);   // 太短的拉长到 6 像素
            float ex = (a1.x - a0.x) * k, ey = len > 0.1f ? (a1.y - a0.y) * k : 3.f;
            a0.x -= ex; a0.y -= ey; a1.x += ex; a1.y += ey; }
        ImColor col = b.surv ? ImColor(60, 230, 90, 210) : ImColor(255, 200, 0, 210);   // 只画天线区间：黄；穿过求生者：绿
        Draw->AddLine(a0, a1, col, th);
    }

    // 落点预测(喷射中)：只画终点小球，按结果上色(青机身/黄天线/红墙地/灰没落地)，黑边便于在亮背景上看清
    if (r.end_kind != 0) {
        ImVec2 e;
        if (wax_proj_pt(r.end, e)) {
            ImColor ec = r.end_kind == 1 ? ImColor(0, 210, 255, 255) : r.end_kind == 2 ? ImColor(255, 200, 0, 255)
                       : r.end_kind == 3 ? ImColor(255, 60, 60, 255) : ImColor(160, 160, 160, 255);
            Draw->AddCircleFilled(e, 10.f, ec);
            Draw->AddCircle(e, 10.f, ImColor(0, 0, 0, 220), 0, 2.f);
        }
    }

}

// 某角色的准星：存过的用存的，没有就用蜡像师的，再没有就用默认(位置 = 游戏蜡枪标记，样式 = 十字加点)
static XhairCfg xhair_for(int pid)
{
    auto it = g_xhair.find(pid);
    if (it != g_xhair.end() && it->second.x >= 0 && it->second.y >= 0) return it->second;
    it = g_xhair.find(WaxAim::WAX_PID);
    if (it != g_xhair.end() && it->second.x >= 0 && it->second.y >= 0) return it->second;
    XhairCfg d; d.x = px; d.y = py * 2.f * WAX_MARK_Y;
    return d;
}

// 换角色时套用该角色的准星(角色 = g_cam_ctrl.unit.pid，WaxAim 线程每帧读)
static void xhair_follow_role()
{
    int pid = WaxAim::status().pid;
    if (pid <= 0 || pid == g_xhair_pid) return;
    g_xhair_pid = pid;
    g_xh = xhair_for(pid);
    WaxAim::aim_x = g_xh.x; WaxAim::aim_y = g_xh.y;
}

static const char *xhair_style_name(int s)
{
    switch (s) {
        case XH_CROSS_DOT:  return T("十字+点", "Cross + dot");
        case XH_CROSS:      return T("十字", "Cross");
        case XH_DOT:        return T("圆点", "Dot");
        case XH_CIRCLE:     return T("圆圈", "Circle");
        case XH_CIRCLE_DOT: return T("圆圈+点", "Circle + dot");
        case XH_T:          return T("T 形", "T-shape");
    }
    return "?";
}

// ---- 准星(所有角色通用，默认关)：画在 WaxAim::aim_x/aim_y，白色带黑边 ----
// 大小 = 臂长(十字/T)或半径(圆圈)，粗细 = 线宽；十字中间留空 = 大小 × 5/12(默认 12 → 5，同原来)
static void draw_crosshair_at(ImDrawList *Draw, float mx, float my, const XhairCfg &c)
{
    const ImColor fg(255, 255, 255, 235), bg(0, 0, 0, 200);
    const float len = c.size, gap = c.size * 5.f / 12.f, th = c.thick;
    const float dot = th * 0.8f;                                  // 默认粗细 2 → 点半径 1.6，同原来
    const ImVec2 d[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int pass = 0; pass < 2; pass++) {                        // 先画黑边再画白线，亮暗背景都看得清
        ImColor col = pass ? fg : bg;
        float w = pass ? th : th + 2.f;
        float r_dot = pass ? dot : dot + 1.2f;
        bool arms = c.style == XH_CROSS_DOT || c.style == XH_CROSS || c.style == XH_T;
        if (arms)
            for (int k = 0; k < 4; k++) {
                if (c.style == XH_T && k == 3) continue;          // T 形不画上臂
                const ImVec2 &v = d[k];
                Draw->AddLine({mx + v.x * gap, my + v.y * gap}, {mx + v.x * (gap + len), my + v.y * (gap + len)}, col, w);
            }
        if (c.style == XH_CIRCLE || c.style == XH_CIRCLE_DOT)
            Draw->AddCircle({mx, my}, len, col, 0, w);
        if (c.style == XH_CROSS_DOT || c.style == XH_CIRCLE_DOT || c.style == XH_T)
            Draw->AddCircleFilled({mx, my}, r_dot, col);
        if (c.style == XH_DOT)
            Draw->AddCircleFilled({mx, my}, pass ? len * 0.35f : len * 0.35f + 1.2f, col);
    }
}

static void draw_crosshair(ImDrawList *Draw)
{
    const float mx = WaxAim::aim_x >= 0 ? WaxAim::aim_x : px, my = WaxAim::aim_y >= 0 ? WaxAim::aim_y : py * 2.f * WAX_MARK_Y;
    draw_crosshair_at(Draw, mx, my, g_xh);
}

void Draw_Main(ImDrawList *Draw){
    PySelf::set_copycat_book(copycat_mode);        // 身份簿只在开关打开时记，见 PySelf.h
    PyWaterFight::set_enabled(waterfight_mode);    // 林中童话关着时它的线程不读内存，见 PyWaterFight.h
    PyItems::set_enabled(show_draw_Prop);          // 道具白名单同理，见 PyItems.h
    if (libbase == 0 || read_state == 0) return;  // 数据未就绪，跳过本帧绘制
    xhair_follow_role();                           // 准星位置跟着当前角色走
    int char_count = 0;

    // ---- 自身锚点：引擎自己持有的答案。相机深度启发式兜底已停用，这是唯一来源 ----
    // g_cam_ctrl.unit 就是"当前视角/操控的单位"，切到机械玩偶/梦之信徒时会跟着换，
    // 所以这里每帧重取：一旦换了操控对象，自身锚点立刻跟上。
    // (不能用 g_unit —— 那是"我的主角色"，切从属时纹丝不动，会高亮错人。见 PySelf.h)
    uint64_t auth_self = 0;
    const bool has_auth_self = PySelf::anchor(auth_self);
    if (has_auth_self) {
        self_obj = (uintptr_t)auth_self;
        int c = PySelf::camp();
        if (c == 1 || c == 2) self_camp = (uintptr_t)c;
    }

    // ---- VP 矩阵：每一跳都校验，读出来的 16 个数也校验 ----
    // 原来是 getPtr64(getPtr64(..)+0xa58)+0x2c0 一步到位：任一跳读成 0(内存压力下驱动静默返回 0，
    // 或跨对局时 +0xa58 真的为空)就得到 0x2c0 这种地址，后面 vm_readv 失败也没人看返回值。
    // 现在校验不过就不更新 Matrix/matrix[]，沿用上一帧，见 MATRIX_STALE_MAX。
    // 2026-10-30：锁的是签名不是槽位。当前槽位读不出来、或读出来但锚点投影不对(真值不过)，当帧就在签名窗口里重选：
    //   找到通过真值的 -> 立刻切过去；当前读不出来时，只要有结构通过的也先切(比停止投影强)；
    //   过场/合照/结算时相机不跟人，所有候选真值一起不过 -> 不切、照用当前的。找不到就降频到每 10 帧试一次。
    //   局内连续约 5 秒一个都读不出来才请求读线程重扫 bss(局外不请求)。没有锚点(大厅/准备)时只做结构校验
    bool matrix_fresh = false;
    {
        static int retry_wait = 0, dead_rounds = 0;
        float anc[3];
        bool has_truth = false;
        if (has_auth_self) {
            uint64_t coor = getPtr64(auth_self + 0x28);
            has_truth = is_user_ptr(coor) && vm_readv(coor + 0xa0, anc, 12) && !(anc[0] == 0 && anc[2] == 0);
        }
        uint64_t cur_slot = libbase + MatrixOffset;
        uint64_t mobj = 0;
        float tmp[16];
        bool cur_ok = read_matrix_root(getPtr64(cur_slot), mobj, tmp);
        bool cur_true = cur_ok && (!has_truth || anchor_in_view(tmp, anc));
        if (cur_true) {
            retry_wait = 0;
            dead_rounds = 0;
        } else if (retry_wait > 0) {
            retry_wait--;
        } else {
            uint64_t ns = 0, nm = 0;
            float nt[16];
            int r = find_matrix_slot(cur_slot, has_truth ? anc : nullptr, ns, nm, nt);
            if (r == 2 || (r == 1 && !cur_ok)) {
                printf("[矩阵切换] 槽位 0x%lx -> 0x%lx(%s)\n", MatrixOffset, (long)(ns - libbase),
                       r == 2 ? (has_truth ? "真值通过" : "结构通过") : "当前读不出");
                MatrixOffset = (long)(ns - libbase);
                mobj = nm;
                memcpy(tmp, nt, sizeof(tmp));
                cur_ok = true;
            }
            if (r != 2) retry_wait = 10;
            // 只在局内(有锚点)累计：局外这根下面本来就没有主相机矩阵，读不出是常态；
            // 而签名在 bss 里一个进程内不动，局外重扫只会找回同一组槽位(两份日志几百次都是)。没锚点走 else 清零，防上一局的计数带进下一局
            if (r == 0 && !cur_ok && has_auth_self) {
                if (++dead_rounds >= 30) { g_rescan_req = true; dead_rounds = 0; }
            } else {
                dead_rounds = 0;
            }
        }
        if (cur_ok) {
            Matrix = mobj + 0x2c0;
            memcpy(matrix, tmp, sizeof(matrix));
            matrix_fresh = true;
        }
    }
    if (matrix_fresh) g_matrix_stale = 0;
    else if (g_matrix_stale <= MATRIX_STALE_MAX) g_matrix_stale++;
    const bool matrix_usable = g_matrix_stale <= MATRIX_STALE_MAX;
    
    // ---- 红夫人镜面：两个来源，先放下的镜子，其次准备阶段的预览镜子 ----
    // (1) 镜子已放下：本体与镜中红夫人连线的垂直平分线(已用 Python 侧 MaryMirrorUnit 验证)。
    //     镜子没放时镜中红夫人停在 y≈-1000/-2000，Z>=-300 挡掉。
    // (2) 准备放镜(SkillMaryPlaceMirrorPrepare)：镜中红夫人还在地下，但预览镜子已经可见。
    //     预览镜子的 objcoor 是 3x3 旋转(+0x78 起、每行 12 字节) + 位置(+0xa0)：
    //       +0x90/+0x98 = 局部 Z 轴 = 投掷方向 = 镜面法向(与 Python rtc_model.world_transformation 第 3 行一致)
    //     可见位 +0x73 只在准备阶段为 1。
    mirror = false;
    // 来源1(最优先): Python MaryMirrorUnit(246) 的权威镜面点 + 法向(2026-10-18 probe2 实测布局/时机)。
    //   放下即可用、放下后平面固定不抖；且比下面"本体+247 垂直平分线"早一帧(那一帧 247 镜中红夫人
    //   还在地下、老来源算不出) —— 正好补上预览→放出的衔接空窗。读不到(没放/非红夫人局)才退回老来源。
    {
        float mpx, mpz, mnx, mnz;
        if (redqueenmod && PyProgress::mirror_plane(mpx, mpz, mnx, mnz)) {
            mirror_line_x1 = mpx;        mirror_line_y1 = mpz;
            mirror_line_x2 = mpx - mnz;  mirror_line_y2 = mpz + mnx;   // 沿镜面方向 = 法向的垂线
            mirror = true;
        }
    }
    bool body_valid = false, mirror_valid = false;
    if (redqueen_obj != 0) {
        uintptr_t redqueen_coor = getPtr64(redqueen_obj + 0x28);
        if (redqueen_coor != 0) {
            redqueen_x = getFloat(redqueen_coor + 0xa0);
            redqueen_z = getFloat(redqueen_coor + 0xa4);
            redqueen_y = getFloat(redqueen_coor + 0xa8);
            body_valid = redqueen_z >= -300 && redqueen_x != 0 && redqueen_y != 0;
        }
    }
    if (redqueen_mirror_obj != 0) {
        uintptr_t mirror_coor = getPtr64(redqueen_mirror_obj + 0x28);
        if (mirror_coor != 0) {
            redqueen_mirror_x = getFloat(mirror_coor + 0xa0);
            redqueen_mirror_z = getFloat(mirror_coor + 0xa4);
            redqueen_mirror_y = getFloat(mirror_coor + 0xa8);
            mirror_valid = redqueen_mirror_z >= -300 && redqueen_mirror_x != 0 && redqueen_mirror_y != 0;
        }
    }
    if (!mirror && body_valid && mirror_valid) {   // 来源2(兜底): 本体+247 垂直平分线。246 没给出时才用
        float mx = (redqueen_x + redqueen_mirror_x) / 2.0f, my = (redqueen_y + redqueen_mirror_y) / 2.0f;
        float dx = redqueen_mirror_x - redqueen_x,      dy = redqueen_mirror_y - redqueen_y;   // 法向
        if (dx * dx + dy * dy > 1e-4f) {
            mirror_line_x1 = mx;      mirror_line_y1 = my;
            mirror_line_x2 = mx - dy; mirror_line_y2 = my + dx;                              // 沿镜面方向
            mirror = true;
        }
    }
    if (!mirror && mirror_preview_obj != 0) {
        uint8_t visible = 0;
        vm_readv(mirror_preview_obj + 0x73, &visible, 1);
        uintptr_t cp = getPtr64(mirror_preview_obj + 0x28);
        if (visible == 1 && cp != 0) {
            float px0 = getFloat(cp + 0xa0), pz0 = getFloat(cp + 0xa4), py0 = getFloat(cp + 0xa8);
            float nx = getFloat(cp + 0x90), ny = getFloat(cp + 0x98);
            float norm_sq = nx * nx + ny * ny;
            if (px0 != 0 && py0 != 0 && pz0 >= -300 && norm_sq > 0.9f && norm_sq < 1.1f) {
                mirror_line_x1 = px0;      mirror_line_y1 = py0;
                mirror_line_x2 = px0 - ny; mirror_line_y2 = py0 + nx;
                mirror = true;
            }
        }
    }
    // 预览镜 → 实际放出的衔接空窗修复：某一帧两个来源都算不出镜线(镜中红夫人正从地下升起、预览刚消失)时，
    // 沿用上一帧的有效镜线一小段时间，避免整帧 mirror=false 的黑帧。镜子放下瞬间几乎不动，沿用几帧无害；
    // 超时才真的停。思路同 read_thread 的"读空不发布、沿用上一轮"(SKILL 待办 #19)。
    {
        static bool have_last_mirror = false;
        static float last_ml_x1 = 0, last_ml_y1 = 0, last_ml_x2 = 0, last_ml_y2 = 0;
        static int mirror_stale = 0;
        const int MIRROR_STALE_MAX = 30;   // 约 0.5s@60fps
        if (!redqueenmod) {
            have_last_mirror = false;                     // 非红夫人模式不保留，防切回来用到陈旧镜线
        } else if (mirror) {
            have_last_mirror = true; mirror_stale = 0;    // 本帧有新镜线，刷新缓存
            last_ml_x1 = mirror_line_x1; last_ml_y1 = mirror_line_y1;
            last_ml_x2 = mirror_line_x2; last_ml_y2 = mirror_line_y2;
        } else if (have_last_mirror && mirror_stale < MIRROR_STALE_MAX) {
            mirror_stale++;                               // 本帧读空，沿用上一帧
            mirror_line_x1 = last_ml_x1; mirror_line_y1 = last_ml_y1;
            mirror_line_x2 = last_ml_x2; mirror_line_y2 = last_ml_y2;
            mirror = true;
        } else {
            have_last_mirror = false;                     // 超时，真的停
        }
    }
    if (matrix_fresh && !first_matrix_logged){
        first_matrix_logged = true;
        printf("[矩阵]");
        for (int i = 0; i < 16; i++) printf(" %.4f", matrix[i]);
        printf("\n");
    }  // 直接从Matrix读16个float

    // ======== 林中童话(娱乐模式，底层打水仗)：独立渲染，矩阵就绪后在这里一刀切掉所有经典绘制 ========
    // 玩家按 water_fight_identity 动物阵营上色(同阵营两名队友同色)、上动物名下距离、不画射线；
    // 物资(箱子/掉落)白点、水平 10m 去重。数据来自 PyWaterFight(Python 层，AOI 内可见)。
    // 自身坐标取自快照里的 is_self 玩家(WaterFight 玩家 model_path=None，没有经典锚点)。
    if (waterfight_mode) {
        float sx0 = 0, sh0 = 0, sz0 = 0; int self_team = 0;
        bool has_self = PyWaterFight::self_ref(sx0, sh0, sz0, self_team);
        // ---- 玩家 ----
        int pn = PyWaterFight::player_count();
        for (int i = 0; i < pn; i++) {
            PyWaterFight::Player pl;
            if (!PyWaterFight::get_player(i, pl)) continue;
            if (pl.is_self) continue;                                   // 自己不画
            if (self_team != 0 && pl.team == self_team) continue;       // 队友不画(同 water_fight_team)
            float qx = pl.x, qz = pl.h, qy = pl.z;                      // qz = 高度轴
            float cam = matrix[3]*qx + matrix[7]*qz + matrix[11]*qy + matrix[15];
            if (cam <= 0.01f) continue;                                 // 相机背后
            float d = has_self ? sqrt(pow(qx - sx0, 2) + pow(qy - sz0, 2) + pow(qz - sh0, 2)) / dist_scale : 0;
            if (has_self && d >= 300) continue;
            float cx = px + (matrix[0]*qx + matrix[4]*qz + matrix[8]*qy + matrix[12]) / cam * px;
            float cy = py - (matrix[1]*qx + matrix[5]*(qz + 8.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float cw = py - (matrix[1]*qx + matrix[5]*(qz + 28.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float bw = (cy - cw) / 2, bh = cy - cw;                     // 跟实体循环同一套方框算法
            if (bw <= 0) continue;
            float bx1 = cx - bh / 4, by1 = cy - bh / 2;
            char_count++;
            ImColor c = waterfight_color(pl.team);                      // 按队伍上色(8 队)
            const char *nm = waterfight_name(pl.identity);             // label 用动物名
            auto ts = ImGui::CalcTextSize(nm, 0, 25);
            Draw->AddText({bx1 + bw/2 - ts.x/2, by1 - 45}, c, nm);      // 上：动物名
            corner_box({bx1, by1}, {bx1 + bw, by1 + bh}, c, 3, 0, 1.8f);
            if (has_self) {                                             // 下：距离
                char dt[24];
                snprintf(dt, sizeof(dt), T("%d 米", "%d m"), (int)d);
                auto ds = ImGui::CalcTextSize(dt, 0, 25);
                Draw->AddText({bx1 + bw/2 - ds.x/2, by1 + bh + 10}, ImColor(255,200,0,255), dt);
            }
            // 不画射线(用户要求)
        }
        // ---- 物资(箱子/掉落)：白点，水平 10m 去重，只记录真正画出来的 ----
        int ln = PyWaterFight::loot_count();
        static float drawn_x[PyWaterFight::MAX_LOOT], drawn_z[PyWaterFight::MAX_LOOT];
        int drawn = 0;
        const float DEDUP_M = 10.0f;
        for (int i = 0; i < ln; i++) {
            PyWaterFight::Loot lt;
            if (!PyWaterFight::get_loot(i, lt)) continue;
            float qx = lt.x, qz = lt.h, qy = lt.z;
            bool near_other = false;                                    // 跟已画的任意一个水平 <10m 就跳过
            for (int j = 0; j < drawn; j++) {
                float dm = sqrt(pow(qx - drawn_x[j], 2) + pow(qy - drawn_z[j], 2)) / dist_scale;
                if (dm < DEDUP_M) { near_other = true; break; }
            }
            if (near_other) continue;
            float cam = matrix[3]*qx + matrix[7]*qz + matrix[11]*qy + matrix[15];
            if (cam <= 0.01f) continue;
            if (has_self) {
                float d = sqrt(pow(qx - sx0, 2) + pow(qy - sz0, 2) + pow(qz - sh0, 2)) / dist_scale;
                if (d >= 300) continue;
            }
            float scx = px + (matrix[0]*qx + matrix[4]*qz + matrix[8]*qy + matrix[12]) / cam * px;
            float scy = py - (matrix[1]*qx + matrix[5]*qz + matrix[9]*qy + matrix[13]) / cam * py;
            Draw->AddCircleFilled({scx, scy}, 4.0f, ImColor(255,255,255,255));
            if (drawn < PyWaterFight::MAX_LOOT) { drawn_x[drawn] = qx; drawn_z[drawn] = qy; drawn++; }
        }
        return;   // 林中童话独立渲染，后面的经典 HUD/实体循环/骨骼全部不走
    }

    // 模仿者局没有监管，类名兜底会把场景里的 chr\boss\spkantan(磁铁)当成监管报出来，不显示
    if (show_draw_prophet && !PySelf::copycat_active()){
        if (PyScene::in_lobby()){
            // 大厅：不报上一局残留的监管名，改显示运行状态(绿字)
            const char *ok = T("当前运行正常", "Running normally");
            auto ts = ImGui::CalcTextSize(ok, 0, 25);
            Draw->AddText({px-(ts.x/2),130}, color_green, ok);
        } else {
            auto textSize = ImGui::CalcTextSize(prophet_text, 0, 25);
            Draw->AddText({px-(textSize.x/2),130}, color_red, prophet_text);
        }
    }

    // 准备阶段：预知那行下面按座位号列出四个求生者的大天赋，**只在我是监管时显示**(用户要求)
    //   数据来自 g_avatar.final_genius_dict(实时；60 秒差分实测只有它跟着对面换天赋变)，座位号来自 uid2pos，见 PyGenius.h
    //   不再显示监管辅助特质(用户 2026-09-24 决定不要)；也不再看花名册 —— 大厅里那是上一局的旧数据
    //   联合狩猎 8 个求生者：每行 4 个，超过就换第二行(2026-10-25)
    if (show_draw_prophet && PySelf::in_lobby() && PyGenius::prep_as_hunter() && PyGenius::civ_size() > 0) {
        char line[320];
        int len = 0, in_line = 0;
        float line_y = 165;
        int n = PyGenius::civ_size();
        for (int i = 0; i < n && len < (int)sizeof(line) - 48; i++) {
            int seat = 0, pid = 0; uint32_t bits = 0;
            if (!PyGenius::civ_at(i, seat, pid, bits) || bits == 0) continue;
            if (in_line == 4) {                                     // 这一行满 4 个，先画掉
                auto ts1 = ImGui::CalcTextSize(line, 0, 25);
                Draw->AddText({px - (ts1.x / 2), line_y}, ImColor(255, 200, 0, 255), line);
                line_y += 32; len = 0; in_line = 0; line[0] = 0;
            }
            in_line++;
            PyGenius::Info tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.camp = 2; tmp.genius_bits = bits; tmp.genius_state = PyGenius::GENIUS_COMPLETE;
            PyGenius::GeniusLine seg;
            PyGenius::genius_segments(tmp, seg);
            // GeniusLine 是 前/飞轮/后 三段(飞轮单独一段，局内要单独上色)，三段都要拼，漏了中间那段飞轮就没了
            int w = seat ? snprintf(line + len, sizeof(line) - len, T("%s%d号[%s%s%s]", "%s#%d[%s%s%s]"), len ? "  " : "", seat, seg.pre, seg.flywheel, seg.post)
                         : snprintf(line + len, sizeof(line) - len, T("%s?号[%s%s%s]", "%s#?[%s%s%s]"), len ? "  " : "", seg.pre, seg.flywheel, seg.post);
            if (w < 0) break;
            len += w;
            if (len >= (int)sizeof(line)) { len = (int)sizeof(line) - 1; break; }   // 截断了，别让下一轮越界
        }
        if (len > 0) {
            auto ts2 = ImGui::CalcTextSize(line, 0, 25);
            Draw->AddText({px - (ts2.x / 2), line_y}, ImColor(255, 200, 0, 255), line);
        }
    }

    // 自动盖板的触摸点：对准游戏里的交互(放板)按钮
    if (AutoPallet::show_touch_point) {
        ImVec2 tp(AutoPallet::touch_x, AutoPallet::touch_y);
        Draw->AddCircle(tp, 40.0f, ImColor(0, 255, 255, 255), 32, 3.0f);
        if (AutoPallet::jitter_px > 0)   // 自动点击的随机范围
            Draw->AddCircle(tp, (float)AutoPallet::jitter_px, ImColor(255, 255, 0, 255), 24, 2.0f);
        Draw->AddLine({tp.x - 12, tp.y}, {tp.x + 12, tp.y}, ImColor(0, 255, 255, 255), 2.0f);
        Draw->AddLine({tp.x, tp.y - 12}, {tp.x, tp.y + 12}, ImColor(0, 255, 255, 255), 2.0f);
        Draw->AddText({tp.x + 46, tp.y - 12}, ImColor(0, 255, 255, 255), T("盖板触摸点", "Pallet tap point"));
    }

    // ===== 固定信息(屏幕坐标 HUD 汇总，增量叠加，不改上面任何现有逻辑) =====
    // A. 密码机 / 大门 面板(左中)。破译未满 5 台时列有进度(>0)且未破译满的机器；
    //    全部破译完后改画两扇门进度(门数据是现有的 PyProgress 门缓冲)。
    if (fi_gen_on && PyProgress::available()){
        float x = fi_gen_x, y = fi_gen_y, sz = fi_gen_size;
        float bar_w = sz * 7.0f, bar_h = sz * 0.8f, gap = sz * 0.4f, line_h = sz + 6.0f;
        int decoded = PyProgress::decoded_count();
        const int need = PySelf::butcher_count() >= 2 ? 7 : 5;   // 联合狩猎(两个监管)要修 7 台(用户确认)，普通局 5 台
        if (decoded < need){
            int gn = PyProgress::gen_total();
            for (int i = 0; i < gn; i++){
                float p = 0.f;
                if (!PyProgress::get_gen(i, p)) continue;
                if (p <= 0.05f) continue;                    // 没进度的不显示
                if (PyProgress::is_decoded(p)) continue;      // 破译满的消失
                ImColor c = progress_color(p);
                float fill = bar_w * (p / 100.0f);
                if (fill > 0.f) Draw->AddRectFilled({x, y}, {x + fill, y + bar_h}, c);
                Draw->AddRect({x, y}, {x + bar_w, y + bar_h}, ImColor(255,255,255,255));
                char pct[16]; snprintf(pct, sizeof(pct), "%.0f%%", p);
                fi_text(Draw, x + bar_w + gap, y + (bar_h - sz)/2.0f, sz, c, pct);
                y += line_h;
            }
        } else {
            int dn = PyProgress::door_count();
            for (int i = 0; i < dn; i++){
                PyProgress::DoorEntry d;
                if (!PyProgress::get_door(i, d)) continue;
                ImColor c = d.can_open ? progress_color(d.progress) : ImColor(180,180,180,255);
                float fill = bar_w * (d.progress / 100.0f);
                if (fill > 0.f) Draw->AddRectFilled({x, y}, {x + fill, y + bar_h}, c);
                Draw->AddRect({x, y}, {x + bar_w, y + bar_h}, ImColor(255,255,255,255));
                char lbl[24]; snprintf(lbl, sizeof(lbl), T("门 %.0f%%", "Gate %.0f%%"), d.progress);
                fi_text(Draw, x + bar_w + gap, y + (bar_h - sz)/2.0f, sz, c, lbl);
                y += line_h;
            }
        }
    }

    // B. 监管 辅助特质 + 大天赋(默认顶中)。只在局内、监管单位存在时有内容。
    //    大天赋游戏不公开对手监管的，lookup 读到才画、读不到就空着。
    //    联合狩猎两个监管(2026-10-25 用户定)：每个监管一行「[名字] 特质 天赋」，下一个监管换行；
    //    单监管保持原样(特质一行、天赋一行)。女巫只有本体，信徒不进这里
    if (fi_hunter_on && PyScene::in_battle() && PySelf::camp() == 2){   // 我是求生者才看监管天赋
        uint64_t hbs[PySelf::MAX_HUNTERS];
        int hn = PySelf::butcher_bodies(hbs, PySelf::MAX_HUNTERS);
        float sz = fi_hunter_size, line_h = sz + 6.0f, y = fi_hunter_y;
        for (int hi = 0; hi < hn; hi++){
            uint64_t hb = hbs[hi];
            PyGenius::Info gi;
            if (hn >= 2){
                const char *name = "";
                for (int i = 0; i < data_count; i++) if (data[i].obj == hb) { name = data[i].str; break; }
                char tl[48] = "", gl[96] = "";
                ImColor cc(255,255,255,255);
                if (PyGenius::lookup((uintptr_t)hb, 0.f, 0.f, gi)){
                    if (gi.support_trait != 0){
                        PyGenius::trait_line_text(gi, PyGenius::trait_name(gi.support_trait), tl, sizeof(tl));
                        if (PyGenius::cd_ready(gi)) cc = color_green;
                    }
                    PyGenius::GeniusLine seg;
                    PyGenius::genius_segments(gi, seg);
                    snprintf(gl, sizeof(gl), "%s%s%s", seg.pre, seg.flywheel, seg.post);
                }
                if (!name[0] && !tl[0] && !gl[0]) continue;
                const char *parts[3] = {name, tl, gl};
                ImColor cols[3] = {color_red, cc, ImColor(255,200,0,255)};
                float gap = sz * 0.5f, w = 0.f;
                for (int k = 0; k < 3; k++) if (parts[k][0]) w += fi_calc(sz, parts[k]).x + gap;
                float x = fi_hunter_x > 0.f ? fi_hunter_x : px - (w - gap)/2.0f;
                for (int k = 0; k < 3; k++){
                    if (!parts[k][0]) continue;
                    fi_text(Draw, x, y, sz, cols[k], parts[k]);
                    x += fi_calc(sz, parts[k]).x + gap;
                }
                y += line_h;
                continue;
            }
            if (PyGenius::lookup((uintptr_t)hb, 0.f, 0.f, gi)){
                if (gi.support_trait != 0){
                    char tl[48];
                    PyGenius::trait_line_text(gi, PyGenius::trait_name(gi.support_trait), tl, sizeof(tl));
                    if (tl[0]){
                        ImColor cc = PyGenius::cd_ready(gi) ? color_green : ImColor(255,255,255,255);
                        ImVec2 ts = fi_calc(sz, tl);
                        float x = fi_hunter_x > 0.f ? fi_hunter_x : px - ts.x/2.0f;
                        fi_text(Draw, x, y, sz, cc, tl);
                        y += line_h;
                    }
                }
                PyGenius::GeniusLine seg;
                PyGenius::genius_segments(gi, seg);
                if (seg.pre[0] || seg.flywheel[0] || seg.post[0]){
                    char gl[96];
                    snprintf(gl, sizeof(gl), "%s%s%s", seg.pre, seg.flywheel, seg.post);
                    ImVec2 ts = fi_calc(sz, gl);
                    float x = fi_hunter_x > 0.f ? fi_hunter_x : px - ts.x/2.0f;
                    fi_text(Draw, x, y, sz, ImColor(255,200,0,255), gl);
                    y += line_h;
                }
            }
        }
    }

    // C. 求生者天赋(右上横排)。联合狩猎(场上 2 个监管)不画(用户 2026-10-25：8 个求生者排不下，不重要)。按绝处逢生三态着色(没带=白/带了没用=绿/已用=灰)。
    //    ★ 带锁存：求生者死亡/逃离后从 data[] 消失，若直接不画会让横排左移、对齐乱。
    //    所以按 obj 固定槽位，读到就更新，消失的槽位保留上次文本 —— 一局内槽位数只增不减。
    static uint64_t fic_obj[4] = {0, 0, 0, 0};
    static char     fic_txt[4][128] = {};
    static ImColor  fic_col[4];
    static int      fic_n = 0;
    static bool     fic_prev_on = false;
    {
        bool on_now = (PySelf::camp() == 1) && PyScene::in_battle();   // 进入局内第一帧清锁存(跨局不串台)
        if (on_now && !fic_prev_on) { fic_n = 0; for (int k = 0; k < 4; k++) { fic_obj[k] = 0; fic_txt[k][0] = 0; } }
        fic_prev_on = on_now;
    }
    if (fi_civ_on && PyScene::in_battle() && PySelf::camp() == 1 && PySelf::butcher_count() < 2){   // 我是监管才看求生者天赋
        for (int i = 0; i < data_count; i++){
            if (data[i].camp != 2) continue;
            if (PySelf::body_set_ready() && !PySelf::is_body(data[i].obj)) continue;
            if (PySelf::body_is_mirror(data[i].obj)) continue;   // 约瑟夫镜世界：镜像投影不进天赋HUD(否则 4 真 4 镜像重复)
            PyGenius::Info gi;
            if (!PyGenius::lookup(data[i].obj, 0.f, 0.f, gi)) continue;
            PyGenius::GeniusLine seg;
            PyGenius::genius_segments(gi, seg);
            char gl[128];
            snprintf(gl, sizeof(gl), "%s%s%s", seg.pre, seg.flywheel, seg.post);
            if (!gl[0]) continue;
            ImColor c;
            switch (PyGenius::desperate_state(gi)){
                case PyGenius::DESPERATE_AVAILABLE: c = color_green; break;
                case PyGenius::DESPERATE_USED:      c = ImColor(140,140,140,255); break;
                default:                            c = ImColor(255,255,255,255); break;
            }
            int slot = -1;                                  // 找已有槽(按 obj)，没有就开新槽
            for (int k = 0; k < fic_n; k++) if (fic_obj[k] == data[i].obj) { slot = k; break; }
            if (slot < 0 && fic_n < 4) { slot = fic_n++; fic_obj[slot] = data[i].obj; }
            if (slot >= 0) { snprintf(fic_txt[slot], sizeof(fic_txt[slot]), "%s", gl); fic_col[slot] = c; }
        }
        // 反向画(求生1 对齐求生1)；死亡的槽位保留文本 -> 不会左移、对齐不乱
        float sz = fi_civ_size, x = fi_civ_x, y = fi_civ_y;
        for (int k = fic_n - 1; k >= 0; k--){
            if (!fic_txt[k][0]) continue;
            fi_text(Draw, x, y, sz, fic_col[k], fic_txt[k]);
            x += fi_calc(sz, fic_txt[k]).x + fi_civ_gap;
        }
    }

    // 以上是屏幕坐标的 HUD(预知/花名册/最后一台)，不依赖矩阵；以下全部要投影
    if (!matrix_usable) return;

    // ---- 自动盖板判定范围：附近立着的板子，按若辰的矩形(或小丑拉锯时的圆)画在地上，命中的那块标红 ----
    if (AutoPallet::show_range) {
        AutoPallet::RangeRect rects[AutoPallet::MAX_RECTS];
        int nr = AutoPallet::ranges(rects, AutoPallet::MAX_RECTS);
        auto project = [&](float x, float y, float h, ImVec2 &out) -> bool {
            float cam = matrix[3]*x + matrix[7]*h + matrix[11]*y + matrix[15];
            if (cam <= 0.01f) return false;
            out.x = px + (matrix[0]*x + matrix[4]*h + matrix[8]*y + matrix[12]) / cam * px;
            out.y = py - (matrix[1]*x + matrix[5]*h + matrix[9]*y + matrix[13]) / cam * py;
            return true;
        };
        for (int i = 0; i < nr; i++) {
            const AutoPallet::RangeRect &r = rects[i];
            ImColor col = r.candidate ? ImColor(255, 60, 60, 255) : ImColor(255, 220, 0, 220);
            if (r.r > 0) {                                   // 圆形判定：画 24 边形
                ImVec2 pts[24]; bool ok = true;
                for (int k = 0; k < 24 && ok; k++) {
                    float ang = k * 6.2831853f / 24;
                    ok = project(r.cx + r.r * cosf(ang), r.cy + r.r * sinf(ang), r.cz, pts[k]);
                }
                if (ok) Draw->AddPolyline(pts, 24, col, ImDrawFlags_Closed, 2.5f);
                continue;
            }
            float vx = -r.uy, vy = r.ux;                     // 垂直方向
            float cx[4] = { r.a,  r.a, -r.a, -r.a};
            float cy[4] = { r.b, -r.b, -r.b,  r.b};
            ImVec2 pts[4]; bool ok = true;
            for (int k = 0; k < 4 && ok; k++)
                ok = project(r.cx + cx[k] * r.ux + cy[k] * vx, r.cy + cx[k] * r.uy + cy[k] * vy, r.cz, pts[k]);
            if (ok) Draw->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 2.5f);
        }
    }

    // ---- 大门开门进度 ----
    // 大门**不走下面那个实体循环**：getscene() 只认 prop_76/sender，大门那条分支收不到东西
    // (原版认 6 个类名，Name.h 重构时缩成 2 个，门/箱/椅三条分支就成了孤儿)。
    // 所以这里直接拿 Python 侧的 model+0x20 场景对象自己投影。
    // 也不能靠场景侧判据补救：实测两扇门的类名都不一样(prop_30 / wooddoor01a)，
    // 而且 +0x240 两扇都是 0 —— "最可靠的存在性判据"在大门上失效。
    if (show_draw_Door){
        for (int i = 0; i < PyProgress::door_count(); i++){
            PyProgress::DoorEntry door;
            if (!PyProgress::get_door(i, door)) continue;
            if (PyProgress::door_opened(door)) continue;          // 已经开了的不用再画
            if (!door.can_open) continue;                         // 还没通电(不可开)的不画

            uintptr_t coor_ptr = getPtr64((uintptr_t)door.scene_obj + 0x28);
            if (coor_ptr == 0) continue;
            float mx = getFloat(coor_ptr + 0xa0);
            float mz = getFloat(coor_ptr + 0xa4);          // +0xa4 是高度
            float my = getFloat(coor_ptr + 0xa8);
            if (mx == 0 && my == 0) continue;

            float cam = matrix[3]*mx + matrix[7]*mz + matrix[11]*my + matrix[15];
            if (cam <= 0.01f) continue;                    // 在相机背后
            float sx = px + (matrix[0]*mx + matrix[4]*mz + matrix[8]*my + matrix[12]) / cam * px;
            float sy = py - (matrix[1]*mx + matrix[5]*(mz+8.5f) + matrix[9]*my + matrix[13]) / cam * py;

            int meters = (int)(sqrt(pow(mx - Z.X, 2) + pow(my - Z.Y, 2) + pow(mz - Z.Z, 2)) / dist_scale);
            char label[64];
            if (door.is_opening)      snprintf(label, sizeof(label), "[%.1f%%]", door.progress);
            else                snprintf(label, sizeof(label), "[%.1f%%]",  door.progress);   // 不可开时按灰色画，见下

            // 有进度就在文字上方画一条，跟密码机那套一致
            if (door.progress > 0.05f){
                const float bar_w = 120.0f, bar_h = 10.0f;
                float bx = sx - bar_w / 2.0f, by = sy - bar_h - 3.0f;
                Draw->AddRectFilled({bx, by}, {bx + bar_w * (door.progress / 100.0f), by + bar_h}, progress_color(door.progress));
                Draw->AddRect({bx, by}, {bx + bar_w, by + bar_h}, ImColor(255,255,255,255));
            }
            auto ts = ImGui::CalcTextSize(label, 0, 25);
            Draw->AddText({sx - ts.x/2.0f, sy}, door.can_open ? progress_color(door.progress) : ImColor(180,180,180,255), label);
        }
    }

    if (matrix_usable) draw_wax_aim(Draw);   // 蜡像师喷蜡瞄准线/落点(不是蜡像师时只更新视线朝向)
    if (show_crosshair) draw_crosshair(Draw);

    // ---- 密码机破译进度：按 Python 侧真实世界列表画(同大门)，不走 data[] 和 is_real_generator ----
    // 约瑟夫局每进出一次镜世界，游戏对密码机 set_visible → lod_callback 切网格，几次之后场景侧判据会挡掉真实密码机，
    // 场景内标记消失而 HUD 正常(2026-10-30)。镜像密码机在 PyProgress 里就没收，这里也就不会画。
    // 坐标优先读场景对象(每帧实时)，没有场景对象时用 position。PyProgress 不可用时实体循环里的老分支兜底
    if (show_draw_secret_mechine && PyProgress::available()){
        for (int i = 0; i < PyProgress::gen_total(); i++){
            PyProgress::Entry g;
            if (!PyProgress::get_gen_entry(i, g)) continue;
            if (PyProgress::is_decoded(g.progress)) continue;    // 破译完的机器本身和进度都不画
            float mx = g.x, mz = g.z, my = g.y;                   // x / 高度 / y
            if (g.scene_obj){
                uintptr_t coor_ptr = getPtr64((uintptr_t)g.scene_obj + 0x28);
                float c[3];
                if (coor_ptr && vm_readv(coor_ptr + 0xa0, c, 12) && !(c[0] == 0 && c[2] == 0)){
                    mx = c[0]; mz = c[1]; my = c[2];
                }
            }
            if (mx == 0 || my == 0) continue;

            float cam = matrix[3]*mx + matrix[7]*mz + matrix[11]*my + matrix[15];
            if (cam <= 0.01f) continue;                    // 在相机背后
            float gdist = sqrt(pow(mx - Z.X, 2) + pow(my - Z.Y, 2) + pow(mz - Z.Z, 2)) / dist_scale;
            if (gdist >= 300) continue;
            float sx = px + (matrix[0]*mx + matrix[4]*mz + matrix[8]*my + matrix[12]) / cam * px;
            float sy = py - (matrix[1]*mx + matrix[5]*(mz+8.5f) + matrix[9]*my + matrix[13]) / cam * py;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << gdist;
            std::string dist_text = " " + oss.str() + T(" 米", " m");
            char head[24];
            snprintf(head, sizeof(head), "[%.1f%%]", g.progress);
            // 两段分开上色：头用进度色，距离在"我是蜡像师且最大仰角能附着"时变绿(WaxAim.h，替代原来 61~63 米)
            auto hs = ImGui::CalcTextSize(head, 0, 25);
            auto ds = ImGui::CalcTextSize(dist_text.c_str(), 0, 25);
            float x0 = sx - (hs.x + ds.x) / 2.0f;
            Draw->AddText({x0, sy}, progress_color(g.progress), head);
            Draw->AddText({x0 + hs.x, sy},
                WaxAim::gen_green(g.scene_obj) ? color_green : ImColor(255, 255, 255, 255),
                dist_text.c_str());
            // 进度条画在文字上方；进度为 0 时没有意义，不画
            if (g.progress > 0.05f){
                const float bar_w = 120.0f, bar_h = 10.0f;
                float bx = sx - bar_w / 2.0f, by = sy - bar_h - 3.0f;
                Draw->AddRectFilled({bx, by}, {bx + bar_w * (g.progress / 100.0f), by + bar_h}, progress_color(g.progress));
                Draw->AddRect({bx, by}, {bx + bar_w, by + bar_h}, ImColor(255,255,255,255));
            }
        }
    }

    // ---- 护肘：手持时不挂模型(model_bind_type!=0，游戏往主人模型上 add_mesh)，场景里没有跟着人走的对象，
    // 按主人本体的坐标画(2026-10-30，见 PyItems.h)。配表里 model_bind_type!=0 的只有护肘 63/2004。
    // 地上的护肘(没主人)模型会被放回场景，走实体循环正常画
    if (show_draw_Prop && PyItems::available()){
        const char *elbow_name = getprop("h55_pendant_huzhou");
        for (int i = 0; elbow_name && i < PyItems::count(); i++){
            PyItems::Entry it;
            if (!PyItems::get(i, it)) continue;
            if (!it.owned || it.bind_type == 0 || it.owner_body == 0) continue;
            if (item_is_mine(it, auth_self)) continue;
            uintptr_t coor_ptr = getPtr64((uintptr_t)it.owner_body + 0x28);
            float c[3];
            if (!coor_ptr || !vm_readv(coor_ptr + 0xa0, c, 12)) continue;
            float mx = c[0], mz = c[1], my = c[2];                // x / 高度 / y
            if (mx == 0 || my == 0 || mz <= -300) continue;

            float cam = matrix[3]*mx + matrix[7]*mz + matrix[11]*my + matrix[15];
            if (cam <= 0.01f) continue;                    // 在相机背后
            float edist = sqrt(pow(mx - Z.X, 2) + pow(my - Z.Y, 2) + pow(mz - Z.Z, 2)) / dist_scale;
            if (edist >= 300) continue;
            float sx = px + (matrix[0]*mx + matrix[4]*mz + matrix[8]*my + matrix[12]) / cam * px;
            float sy = py - (matrix[1]*mx + matrix[5]*(mz+8.5f) + matrix[9]*my + matrix[13]) / cam * py;

            std::string s = elbow_name;                    // 在人身上，不带距离(人自己的标签有)
            auto ts = ImGui::CalcTextSize(s.c_str(), 0, 25);
            Draw->AddText({sx - ts.x/2.0f, sy}, ImColor(255,200,0,255), s.c_str());
        }
    }

    // ---- 模仿者模式：按身份簿里每个活人的 unit.position 自己投影(学若辰，2026-09-26) ----
    // 不走下面的实体循环：假面舞会时模型换成石像，按 model+0x20 配对会整个配不上。
    // 位置从 Python 单位上读，换不换模型都一样。坐标拿不到的人(没校准/这轮读失败超过 300ms)
    // 不在这里画，由实体循环按场景对象画(旧行为)，两边不会重复
    if (copycat_mode) {
        PySelf::CopycatBook book = PySelf::copycat_book();
        for (int k = 0; k < PySelf::MAX_COPYCAT; k++) {
            const PySelf::CopycatSeat &st = book.seat[k];
            if (!st.known || st.dead) continue;
            if (book.self_seat == k + 1) continue;                      // 自己不画
            float qx = 0, qz = 0, qy = 0;
            if (!PySelf::copycat_seat_pos(k + 1, qx, qz, qy)) continue;

            float cam = matrix[3]*qx + matrix[7]*qz + matrix[11]*qy + matrix[15];
            if (cam <= 0.01f) continue;                                 // 在相机背后
            float d = sqrt(pow(qx - Z.X, 2) + pow(qy - Z.Y, 2) + pow(qz - Z.Z, 2)) / dist_scale;
            if (d >= 300) continue;
            if (self_obj != 0 && d < 1.0f) continue;                    // 自己的号码没标出来时，贴身那个就是自己
            float cx = px + (matrix[0]*qx + matrix[4]*qz + matrix[8]*qy + matrix[12]) / cam * px;
            float cy = py - (matrix[1]*qx + matrix[5]*(qz + 8.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float cw = py - (matrix[1]*qx + matrix[5]*(qz + 28.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float bw = (cy - cw) / 2, bh = cy - cw;                     // 跟实体循环同一套方框算法
            if (bw <= 0) continue;
            float bx1 = cx - bh / 4, by1 = cy - bh / 2;
            char_count++;

            char text[48];
            copycat_label(st.identity, k + 1, text, sizeof(text));
            ImColor c = copycat_color(st.identity);
            auto ts = ImGui::CalcTextSize(text, 0, 25);
            Draw->AddText({bx1 + bw/2 - ts.x/2, by1 - 45}, c, text);
            if (show_draw_Rect)
                corner_box({bx1, by1}, {bx1 + bw, by1 + bh}, c, 3, 0, 1.8f);
            if (show_draw_Distance) {
                char dt[24];
                snprintf(dt, sizeof(dt), T("%d 米", "%d m"), (int)d);
                auto ds = ImGui::CalcTextSize(dt, 0, 25);
                Draw->AddText({bx1 + bw/2 - ds.x/2, by1 + bh + 10}, ImColor(255,200,0,255), dt);
            }
            // 模仿者模式不画射线(用户要求)
        }

        // 食人鬼 / 导演分身 / 导演傀儡：没号码没身份，进不了身份簿，按 unit.position 画、打固定标签。
        // 食人鬼橙、导演类紫。普通模仿者局收不到这些，en=0 不画。
        PySelf::ExtraUnit ex[PySelf::MAX_EXTRA];
        int en = PySelf::copycat_extra(ex, PySelf::MAX_EXTRA);
        for (int k = 0; k < en; k++) {
            float qx = ex[k].x, qz = ex[k].z, qy = ex[k].y;
            float cam = matrix[3]*qx + matrix[7]*qz + matrix[11]*qy + matrix[15];
            if (cam <= 0.01f) continue;
            float d = sqrt(pow(qx - Z.X, 2) + pow(qy - Z.Y, 2) + pow(qz - Z.Z, 2)) / dist_scale;
            if (d >= 300) continue;
            float cx = px + (matrix[0]*qx + matrix[4]*qz + matrix[8]*qy + matrix[12]) / cam * px;
            float cy = py - (matrix[1]*qx + matrix[5]*(qz + 8.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float cw = py - (matrix[1]*qx + matrix[5]*(qz + 28.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float bw = (cy - cw) / 2, bh = cy - cw;
            if (bw <= 0) continue;
            float bx1 = cx - bh / 4, by1 = cy - bh / 2;
            char_count++;
            const char *lbl = PySelf::copycat_extra_label(ex[k].label);
            ImColor c = ex[k].label == 0 ? ImColor(255,140,0,255) : ImColor(200,80,255,255);
            auto ts = ImGui::CalcTextSize(lbl, 0, 25);
            Draw->AddText({bx1 + bw/2 - ts.x/2, by1 - 45}, c, lbl);
            if (show_draw_Rect)
                corner_box({bx1, by1}, {bx1 + bw, by1 + bh}, c, 3, 0, 1.8f);
            if (show_draw_Distance) {
                char dt[24];
                snprintf(dt, sizeof(dt), T("%d 米", "%d m"), (int)d);
                auto ds = ImGui::CalcTextSize(dt, 0, 25);
                Draw->AddText({bx1 + bw/2 - ds.x/2, by1 + bh + 10}, ImColor(255,200,0,255), dt);
            }
        }
    }

    // ---- 原生骨骼(抄若辰 sob_3A6074/39F0CC 偏移链,驱动读,零注入;见 NativeBones.h) ----
    // 本帧画出来的骨骼包围盒(屏幕坐标，含头圆)：方框优先用它，保证骨骼一定在框里(2026-11-05)。
    // 原来方框是写死的世界高度(脚底 -1.5~+18.5 单位)，头圆最小半径/上顶量/线宽又是固定像素，远处骨骼会冒出框顶
    g_bone_box_n = 0;
    g_diag_bone_n = 0;
    if (show_draw_Bones && PySelf::available() && !PySelf::is_joseph_match()) {   // 约瑟夫局永远不画骨骼(用户要求,无视开关)
        uint64_t self_m = 0; PySelf::anchor(self_m);
        uint64_t Ms[32];
        int bn = PySelf::copy_bodies(Ms, 32);
        bool hunter_view = (PySelf::camp() == 1);   // 监管视角：被自己扛气球的求生者不画骨骼(同方框过滤)
        for (int u = 0; u < bn; u++) {
            bool is_self = (Ms[u] == self_m);
            if (!is_self && hunter_view && PySelf::is_carried(Ms[u])) continue;
            NativeBones::Skeleton sk;
            if (!NativeBones::read_unit(Ms[u], is_self, sk) || !sk.valid) continue;
            if (is_self) continue;                                // 自身只用于 swap 校准,不画(若辰 ignoreSelfBones)
            // 和其它绘制一样：离自己 <1m 的不画(多是自己身上的残留模型),没自身锚点时不过滤
            uint64_t coor = getPtr64(Ms[u] + 0x28);
            if (self_obj != 0 && coor >= 0x5000000000ULL && coor < 0x8000000000ULL) {
                float c[3];
                if (vm_readv(coor + 0xa0, c, 12)) {
                    float d = sqrt(pow(c[0]-Z.X,2) + pow(c[2]-Z.Y,2) + pow(c[1]-Z.Z,2)) / dist_scale;
                    if (d < 1.0f) continue;
                }
            }
            ImVec2 bp[NativeBones::NUM_TARGET];
            bool bok[NativeBones::NUM_TARGET];
            for (int t = 0; t < NativeBones::NUM_TARGET; t++) {
                bok[t] = false;
                if (!sk.ok[t]) continue;
                float ox = sk.x[t], oy = sk.y[t], oz = sk.z[t];
                float cam = matrix[3]*ox + matrix[7]*oy + matrix[11]*oz + matrix[15];
                if (cam <= 0.01f) continue;                       // 相机背后
                float sx = px + (matrix[0]*ox + matrix[4]*oy + matrix[8]*oz + matrix[12]) / cam * px;
                float sy = py - (matrix[1]*ox + matrix[5]*oy + matrix[9]*oz + matrix[13]) / cam * py;
                bp[t] = ImVec2(sx, sy); bok[t] = true;
            }
            // 监管方红、求生者绿。约瑟夫镜世界里"另一个世界"的人画白色
            // 监管方 = 本体集合里来自 [1]/[236]/[1065] 的单位：联合狩猎两个监管、巡视者、梦之信徒、伊斯幻影都红
            ImU32 bcol;
            if (PySelf::is_other_world(Ms[u]))      bcol = (ImU32)BotBoneColor;
            else if (PySelf::is_hunter_side(Ms[u]))   bcol = (ImU32)ImColor(255, 60, 60, 230);
            else                                      bcol = (ImU32)ImColor(40, 255, 90, 230);
            for (int e = 0; e < NativeBones::NUM_EDGE; e++) {
                int a = NativeBones::EDGES[e][0], b = NativeBones::EDGES[e][1];
                if (a == 0 || b == 0) continue;                   // 头(slot0)单独画,见下
                if (bok[a] && bok[b]) Draw->AddLine(bp[a], bp[b], bcol, 2.2f);
            }
            // 头:引擎颈->头段极短,圆压在颈上不好看。沿 脊椎->颈 的身体朝向把头圆往上顶、脖子接到圆底(不重合)
            if (bok[0]) {
                ImVec2 up(0.f, -1.f);
                float r = 6.0f;
                if (bok[1] && bok[2]) {                            // 颈 + 脊椎 → 身体上方向 + 躯干尺度
                    float dx = bp[1].x - bp[2].x, dy = bp[1].y - bp[2].y;   // 脊椎->颈
                    float L = sqrtf(dx*dx + dy*dy);
                    if (L > 0.5f) { up.x = dx / L; up.y = dy / L; r = L * 0.28f; }  // 头半径≈躯干的 ~1/4(原 0.5 中近距偏大)
                }
                if (r < 3.0f) r = 3.0f;
                if (r > 26.0f) r = 26.0f;
                ImVec2 base = bok[1] ? bp[1] : bp[0];             // 以颈为底
                ImVec2 hc(base.x + up.x * (r + 3.0f), base.y + up.y * (r + 3.0f));
                if (bok[1]) Draw->AddLine(bp[1], ImVec2(hc.x - up.x * r, hc.y - up.y * r), bcol, 2.2f);  // 脖子接圆底
                Draw->AddCircleFilled(hc, r, bcol, 16);
                bone_box_add_circle(Ms[u], hc, r);
            }
            for (int t = 0; t < NativeBones::NUM_TARGET; t++) if (bok[t]) bone_box_add_point(Ms[u], bp[t]);
            float bx1, by1, bx2, by2;
            if (bone_box_get(Ms[u], bx1, by1, bx2, by2) && g_diag_bone_n < 32) g_diag_bone[g_diag_bone_n++] = Ms[u];   // 【临时诊断】本帧画了骨骼的本体
        }
    }

    // ---- 红夫人镜像求生者：画一个求生者关于镜线的镜像(名字/方框/距离/骨骼) ----
    // 抽成 lambda(2026-11-04)：自身那行在循环开头就 continue 掉了，我是求生者时自己的镜像原来画不出来。
    // 现在常规求生者和自身各调一次。会改写 D(反射后的坐标)，调完这一行就不能再用 D
    auto draw_mirror_image = [&](int i) {
            if (getFloat(data[i].obj+0x1a0)==450&&data[i].camp==2){
                std::string ss;
                // 镜线在 Draw_Main 开头算好(放下的镜子 / 准备阶段预览镜子二选一)，这里只做一次关于直线的对称
                float orig_x = D.X, orig_y = D.Y;
                calculate_line_reflection(mirror_line_x1, mirror_line_y1, mirror_line_x2, mirror_line_y2, orig_x, orig_y, &D.X, &D.Y);
                camera = matrix[3] * D.X + matrix[7] * D.Z + matrix[11] * D.Y + matrix[15];
                dist = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2) + pow(D.Z - Z.Z, 2)) / dist_scale;
                if (dist > 60) return;   // 用户要求：过滤 60m 外的镜像求生者(镜像反射点到自身的距离)，别让远处镜像刷屏
        		r_x = px + (matrix[0] * D.X + matrix[4] * D.Z + matrix[8] * D.Y + matrix[12]) / camera * px;
                r_y = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 8.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
                r_w = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 28.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
												
        		W = (r_y - r_w) / 2;	// 宽度
        		H = r_y - r_w;		// 高度
        		X1 = r_x - (r_y - r_w) / 4;	// X1
        		Y1 = r_y - H / 2;	// Y1
        		X2 = X1 + W;		// X2
        		Y2 = Y1 + H;		// Y2
        
                if (W>0){
                    // swap_yz 只在主骨骼轮读自身时校准；若"显示骨骼"关着主轮没跑，这里先拿自身补一次。
                    if (NativeBones::g_swap_yz.load() < 0) {
                        uint64_t self_cal = 0;
                        if (PySelf::anchor(self_cal)) { NativeBones::Skeleton tmp; NativeBones::read_unit(self_cal, true, tmp); }
                    }
                    // 骨骼先读：方框按反射后的骨骼包围盒算(跟普通人物一致)，读不到才用上面写死高度那套
                    NativeBones::Skeleton msk;
                    const bool msk_ok = NativeBones::read_unit(data[i].obj, false, msk) && msk.valid;
                    {
                        float sx1, sy1, sx2, sy2;
                        if (msk_ok && skeleton_screen_box(msk, true, sx1, sy1, sx2, sy2)) {
                            X1 = sx1; Y1 = sy1; X2 = sx2; Y2 = sy2; W = X2 - X1; H = Y2 - Y1;
                        }
                    }

                    ss += data[i].str;
                    auto textSize = ImGui::CalcTextSize(ss.c_str(), 0, 25);
                    Draw->AddText({X1 + W/2-(textSize.x/2),Y1-45}, BotBoneColor, ss.c_str());
                        
                    if (true){   // 方框默认画(不受"方框绘制"开关影响，用户要求)
                        corner_box({X1, Y1},{X2, Y2}, BotBoneColor,3, 0,1.8f);
    			    }
    			        
                    if (show_draw_Distance){
                        std::string mirror_dist_str;
                        mirror_dist_str += std::to_string((int) dist);
                        mirror_dist_str += T(" 米", " m");
                        auto textSize = ImGui::CalcTextSize(mirror_dist_str.c_str(), 0, 25);
                        Draw->AddText({X1 + W/2-(textSize.x/2),Y2+10}, BotBoneColor, mirror_dist_str.c_str());
                    }
                        
                    // 射线不画(用户要求)。
                    // 骨骼默认画(不受"显示骨骼"开关影响)。
                    // 【2026-10-19 重做】此前"方案B"把骨架当刚体整体平移到镜像落点(只反射根、其余骨骼
                    //   保持真人世界偏移 delta=bone-root 原样搬过去)，数学上不是镜像：
                    //   ① 朝向/手性不翻转 —— 真人侧身/背身时镜中姿态方向错；
                    //   ② 平移后的点仍按各自深度投影 —— 镜像落点离相机近时(照镜子最常见)人体尺度被透视放大→变大变形。
                    //   当年"逐根反射→忽大忽小"的归因是错的：镜面反射是等距变换、绝不改变骨架自身尺寸
                    //   (主骨骼轮 1404-1412 同样逐骨骼各自深度投影却不变形)；真凶是旧镜线来源(本体+247/预览)
                    //   每帧抖动。镜线已迁到稳定的 Python MaryMirrorUnit(246)，逐根反射现在安全。
                    //   所以改回"对每根骨骼的水平两轴做真·镜面反射(高度不动)+ 正常投影"：反射后整副骨架落在
                    //   镜面另一侧(照镜子时在相机前方、投得出)，大小/透视天然=真人站在镜像位置，方向自动翻成真镜像。
                    // (swap_yz 校准和读骨骼挪到了本块开头，方框要先用)
                    if (msk_ok) {
                        ImVec2 bp[NativeBones::NUM_TARGET];
                        bool bok[NativeBones::NUM_TARGET];
                        for (int t = 0; t < NativeBones::NUM_TARGET; t++) {
                            bok[t] = false;
                            if (!msk.ok[t]) continue;
                            // 每根骨骼的水平坐标(X, Y)关于镜线做真·镜面反射，高度(oy)不反射。
                            float ox, oz;
                            calculate_line_reflection(mirror_line_x1, mirror_line_y1,
                                                      mirror_line_x2, mirror_line_y2,
                                                      msk.x[t], msk.z[t], &ox, &oz);
                            float oy = msk.y[t];                    // 高度不反射
                            float cam = matrix[3]*ox + matrix[7]*oy + matrix[11]*oz + matrix[15];
                            if (cam <= 0.01f) continue;
                            float sx = px + (matrix[0]*ox + matrix[4]*oy + matrix[8]*oz + matrix[12]) / cam * px;
                            float sy = py - (matrix[1]*ox + matrix[5]*oy + matrix[9]*oz + matrix[13]) / cam * py;
                            bp[t] = ImVec2(sx, sy); bok[t] = true;
                        }
                        for (int e = 0; e < NativeBones::NUM_EDGE; e++) {
                            int a = NativeBones::EDGES[e][0], b = NativeBones::EDGES[e][1];
                            if (a == 0 || b == 0) continue;           // 头单独画
                            if (bok[a] && bok[b]) Draw->AddLine(bp[a], bp[b], BotBoneColor, 2.2f);
                        }
                        if (bok[0]) {                                  // 头圆，抄主骨骼轮的画法
                            ImVec2 up(0.f, -1.f);
                            float r = 6.0f;
                            if (bok[1] && bok[2]) {
                                float dx = bp[1].x - bp[2].x, dy = bp[1].y - bp[2].y;
                                float L = sqrtf(dx*dx + dy*dy);
                                if (L > 0.5f) { up.x = dx / L; up.y = dy / L; r = L * 0.28f; }
                            }
                            if (r < 3.0f) r = 3.0f;
                            if (r > 26.0f) r = 26.0f;
                            ImVec2 base = bok[1] ? bp[1] : bp[0];
                            ImVec2 hc(base.x + up.x * (r + 3.0f), base.y + up.y * (r + 3.0f));
                            if (bok[1]) Draw->AddLine(bp[1], ImVec2(hc.x - up.x * r, hc.y - up.y * r), BotBoneColor, 2.2f);
                            Draw->AddCircleFilled(hc, r, BotBoneColor, 16);
                        }
                    }
                }//判断W
            }
    };

    // 【临时诊断】本帧方框那段走到的对象清零；记下循环开始时读线程的状态(撕裂窗口)
    g_diag_box_n = 0;
    const bool     diag_busy0 = g_scan_busy.load();
    const uint32_t diag_gen0  = g_scan_gen.load();
    const int      diag_count0 = data_count;
    // 镜中红夫人：优先 Python 侧 247 单位的 model+0x20，读不到才用读线程按 +0x6D 位认的那个
    uint64_t mirror_img = PyProgress::mirror_image_obj();
    if (mirror_img == 0) mirror_img = redqueen_mirror_obj;

    for (int i = 0; i < data_count; i++){

        if (strstr(data[i].class_name, "buzz") != NULL)
            continue;//跳过不知所谓的东西
        if (strstr(data[i].class_name, "nvyao.gim") != NULL)
            continue;//跳过女妖蜡烛
        // 三个坐标是连续的 12 字节，一次读完。原来是三次 getFloat = 三次 ioctl，
        // 而每次驱动读约 3µs，30 个实体 60fps 下这一项就占渲染线程约 1% CPU。
        // 读失败时保持 0，和 getFloat 失败返回 0 的行为一致。
        float xyz[3] = {0, 0, 0};
        vm_readv(data[i].objcoor + 0xa0, xyz, 12);
        D.X = xyz[0];
        D.Z = xyz[1];
        D.Y = xyz[2];

        // 已锁定的自身: 只负责判断"还活着没"+持续更新坐标, 不重新参与后面的识别/绘制逻辑
        if (self_obj != 0 && data[i].obj == self_obj) {
            if (!ShouldSkipEntity(data[i]) && !(D.X==0 && D.Y==0) && D.Z>-300) {
                Z.X = D.X; Z.Z = D.Z; Z.Y = D.Y;
            }
            // 我是求生者时自己的镜像也要画(红夫人镜子里的我)。lambda 里自带 camp==2 / 450 判断，监管自身不会画
            if (mirror && redqueenmod && !(D.X==0 && D.Y==0) && D.Z>-300) draw_mirror_image(i);
            continue; // 不管有效无效, 自身都不需要再走下面的常规实体流程
        }

        // 只画角色本体：CPython 侧 units_by_type[1]/[2]/[236]/[1065] 的 unit.model 对应的场景对象(见 PySelf.h 本体集合)。
        // 模仿者局是 [1088] 里的活人(死者和 [1089] 鬼魂不收，所以不画)。
        // 挡掉同名分身副本(_fragrance_image)、另一形态、挂件、时装、魔术师/幻灯师分身；机械玩偶保留。
        // 只管按 player/boss 类名归进 1/2 的对象 —— 类名分类那里另有两个特例(deluosi 鬼魂、火箭挂件)
        // 被故意归成求生者，它们不是任何单位的 model，不能被这层挡掉。
        // 本体集合不可用(准备阶段/大厅/读取失败)时不过滤，照旧按类名画。
        // 大厅/准备阶段(units_by_type 读全了且没有键 1/2)：没有任何玩家单位，场景里的人物对象全是
        // 无宿主的时装挂件(头饰/袖子)，阵营 1/2 整类不画。预知监管在读取循环里算，不受影响
        if ((data[i].camp == 1 || data[i].camp == 2) && PySelf::in_lobby())
            continue;
        // 镜中红夫人在循环后单独画(标「红夫人镜像」)，这里不重复(本体集合不可用时它会被当成普通红夫人画出来)
        if (mirror_img != 0 && data[i].obj == mirror_img)
            continue;
        // 模仿者模式开着且身份簿有内容：按簿子过滤(只画簿子里的活人)。开会阶段 Python 页被换出，
        // 本体集合读不全甚至整个不可用，那时退回按类名画会把尸体、鬼魂、石像全画出来。
        // 拿得到 unit.position 的人已经在上面按坐标画过了，这里跳过，只给坐标读不到的人兜底
        const bool cc_filter = copycat_mode && PySelf::copycat_book_ready();
        if (cc_filter && (data[i].camp == 1 || data[i].camp == 2)
            && (strstr(data[i].class_name, "player") != NULL || strstr(data[i].class_name, "boss") != NULL)) {
            int ident_unused = 0, cc_seat = 0;
            if (!PySelf::copycat_identity(data[i].obj, ident_unused, cc_seat)) continue;
            float ux, uz, uy;
            if (PySelf::copycat_seat_pos(cc_seat, ux, uz, uy)) continue;
        }
        else if ((data[i].camp == 1 || data[i].camp == 2) && PySelf::body_set_ready()
            && (strstr(data[i].class_name, "player") != NULL || strstr(data[i].class_name, "boss") != NULL)
            && !PySelf::is_body(data[i].obj))
            continue;

        if (D.X==0 || D.Y==0){
		    continue;//跳过xy0
		}
		if (D.Z<=-300){
		    continue;//跳过地下
		}
		if (data[i].camp == 1 || data[i].camp == 2) char_count++;
		// 本体是监管时，游戏自己就会显示监管者个体，这里不再重复画任何监管者。
		// 只认 CPython 锚点给的阵营(has_auth_self)：阵营判错会让求生者视角下的监管整个消失，代价远大于多画一个。
		// 联合狩猎(2026-10-25)：队友监管照画，只跳过我自己和我这边的其它监管阵营对象
		if (has_auth_self && self_camp == 1 && data[i].camp == 1 && !PySelf::is_teammate_butcher(data[i].obj)) continue;
		// 本体是监管时，被自己扛在气球上的求生者会糊在自己身上(模型挂在气球挂点)，游戏自己也会显示，
		// 这里不重复画。判据 b_catch(= 监管 carry_uid 指向的人)在 PySelf 里收集，只在监管视角过滤。
		if (has_auth_self && self_camp == 1 && data[i].camp == 2 && PySelf::is_carried(data[i].obj)) continue;
		// 这里原本还读一次 +0x70(jxpd) 并算 cam_dist / niexi_dist，三者都没有任何地方使用，已删。
		// jxpd 那次是每实体每帧一次驱动读，删掉直接省 CPU；另两个只是浮点运算。
		camera = matrix[3] * D.X + matrix[7] * D.Z + matrix[11] * D.Y + matrix[15];
        dist = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2) + pow(D.Z - Z.Z, 2)) / dist_scale;
		r_x = px + (matrix[0] * D.X + matrix[4] * D.Z + matrix[8] * D.Y + matrix[12]) / camera * px;
        r_y = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 8.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
        r_w = py - (matrix[1] * D.X + matrix[5] * (D.Z+ 28.5) + matrix[9] * (D.Y) + matrix[13]) / camera * py;
												
		W = (r_y - r_w) / 2;	// 宽度
		H = r_y - r_w;		// 高度
		X1 = r_x - (r_y - r_w) / 4;	// X1
		Y1 = r_y - H / 2;	// Y1
		X2 = X1 + W;		// X2
		Y2 = Y1 + H;		// Y2
		if (dist>=300){
            continue;
        }
        if (W>0){
            if (Debugging){
                // 调试绘制是独立分支，不走上面任何类别过滤，所以会把每局默认加载的装饰对象
                // 一起画出来(表现为"同一个位置两份不同地址"、约瑟夫相机等)。这里统一挡掉。
                // 判据用 +0x240 而不是活跃位：活跃位是"当前对本机可见"，跟视角走，
                // 求生者视角下大量真实对象的活跃位也是 0，用它会把真东西一起挡掉。
                // 想看全部对象(比如排查新偏移时)，把下面这行注释掉即可。
                if (!entity_exists(data[i].obj)) continue;
                std::string test;
                sprintf(objtext, "%lx", data[i].obj);
                test += " [";
                test += std::to_string((int) dist);
                test += T(" 米]  0x", " m]  0x");
                test += objtext;
                test += T(" [类名] ", " [class] ");
                test += data[i].class_name;
                auto textSize = ImGui::CalcTextSize(test.c_str(), 0, 25);
                Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), test.c_str());
            }

            if (strstr(data[i].class_name, "camera") != NULL && dist < 38){
                // 约瑟夫的相机每局都会默认加载十几个，跟这局有没有约瑟夫无关。
                // 实测那些默认加载的 +0x240 全是 0（7/7），本局真实存在的物件是 2。
                if (!entity_exists(data[i].obj)) continue;
                if (getDword(data[i].obj + 0xa8)==256){
		            continue;//跳过使用过的椅子
		        }
                std::string s;
			    if (show_draw_Camera){                          
                    s += T("[摄影机]", "[Camera]");
                }
                auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
            }
            

		
    		if (data[i].camp==3)
    		{
    		    if (strstr(data[i].class_name, "dm65_scene_prop_30") != NULL){
    			    std::string s;
    		        if (show_draw_Door){
                        s += T("[大门]", "[Exit Gate]");
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
    		    }
    		
    		    else if (strstr(data[i].class_name, "dm65_scene_prop_01") != NULL&&dist<38){
    			    std::string s;
    		        if (show_draw_Box){                          
    		            if (getDword(data[i].obj + 0x148)==0){
    		                continue;//跳过使用过的箱子
    		            }
                        s += T("[道具箱]", "[Chest]");
                    }    
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, color_red, s.c_str());
    		    }

    		    else if (strstr(data[i].class_name, "dm65_scene_gallow") != NULL&&strstr(data[i].class_name, "bashou") == NULL&&dist<38){
    			    std::string s;
    		        if (show_draw_Chair){                          
    		            if (getDword(data[i].obj + 0xa8)==256){
    		                continue;//跳过使用过的椅子
    		            }
                        s += T("[狂欢之椅]", "[Rocket Chair]");
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, color_red, s.c_str());
    		    }
    		
    		    else if (strstr(data[i].class_name, "dm65_scene_prop_76") != NULL){
    			    std::string s;
    		        if (show_draw_Cellar){                                                  
                            s += T("[地窖] ", "[Dungeon] ");
                            s += std::to_string((int) dist);
                            s += T(" 米 ", " m ");
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, color_purple, s.c_str());
    		    }

    	    else if (strstr(data[i].class_name, "sender") != NULL){
    	        // 兜底：PyProgress 可用时密码机在实体循环前按 Python 侧列表画(见那里)，这里只在它不可用时走。
    	        // 判据见上面 is_real_generator() 的注释。
    	        if (show_draw_secret_mechine && !PyProgress::available() && is_real_generator(data[i].obj)){
    	            // 走到这里说明 Python 层不可用，拿不到进度，只画 "[密码机] X.X 米"。
    	            // (原来这里调 PyProgress::lookup 配进度，但它在不可用时恒返回 false，是死代码，2026-11-04 删)
    	            std::ostringstream oss;
    	            oss << std::fixed << std::setprecision(1) << dist;
    	            std::string dist_text = " " + oss.str() + T(" 米", " m");
    	            const char *head = T("[密码机]", "[Cipher]");
    	            // 距离在"我是蜡像师且最大仰角能附着"时变绿(WaxAim.h，替代原来 61~63 米)
    	            auto hs = ImGui::CalcTextSize(head, 0, 25);
    	            auto ds = ImGui::CalcTextSize(dist_text.c_str(), 0, 25);
    	            float x0 = r_x - (hs.x + ds.x) / 2.0f;
    	            Draw->AddText({x0, r_y}, ImColor(255,255,255,255), head);
    	            Draw->AddText({x0 + hs.x, r_y},
    	                WaxAim::gen_green(data[i].obj) ? color_green : ImColor(255, 255, 255, 255),
    	                dist_text.c_str());
    	        }
    	    }
    		}
	
		    if (show_draw_Prop&&data[i].camp==4){
                // 道具模型(h55_pendant_*)按游戏自己的道具清单过滤(PyItems.h，2026-10-30)：
                //   不在 _uid2item 里 = 用完已销毁、场景里残留的模型，不画；
                //   护肘这类 model_bind_type!=0 的手持道具，场景里那个模型停在创建点，不画(下面按主人位置另画)；
                //   自己的(主人是我 / 所在的玩具商包贴在我身上)不画。
                // 清单不可用或这轮没读全、以及不是道具模型的，退回下面的老逻辑。
                // 厂长傀儡/窥视者/巡视者是单位不是道具，模型却也在 h55_pendant 下(skill_creature 1~4 / 12,70 / 15,125)，
                // 不在 _uid2item 里，进白名单会被全挡掉，所以排除。小丑骑的火箭同理，但它在 PyItems 里按单位收了
                const char *cn = data[i].class_name;
                const bool is_pendant = strstr(cn, "h55_pendant") != NULL && strstr(cn, "pendant_puppet") == NULL
                                     && strstr(cn, "pendant_tower") == NULL && strstr(cn, "pendant_patrol") == NULL;
                PyItems::Entry item;
                bool draw_prop;
                bool on_body = false;   // 在人身上(有主人，或所在的玩具商包贴在人身上)：只写名字不带距离
                if (strstr(cn, "trap.gim") != NULL) {
                    // 鹿头夹子：是单位不是道具，不进 _uid2item，也没人拿在手上，不走距离剔除。
                    // 我是监管时不画：游戏本来就显示，而且放夹子时的瞄准预览(VSkillClaspTrapCharge.rtc_model)
                    // 用的是同一个 trap.gim，会被当成夹子画出来
                    draw_prop = !(has_auth_self && self_camp == 1);
                } else if (is_pendant && PyItems::lookup(data[i].obj, item)) {
                    draw_prop = !(item.owned && item.bind_type != 0) && !item_is_mine(item, auth_self);
                    on_body = item.owned || item.attach_uid > 0;
                } else if (is_pendant && PyItems::complete()) {
                    draw_prop = false;
                } else {
                    // 老逻辑：自己身上的道具按距离剔除。必须只算**水平**距离:
                    // 道具是挂在角色骨骼上的(手/胸口)，挂点比角色原点(脚底)高一截，
                    // 实测 h55_pendant_glim(手电筒) 高度差固定 +0.73 米、水平只差 0.36 米。
                    // 三维距离因此恒有 0.7+ 米的底噪，亚米阈值永远不成立 —— 挂件类道具靠
                    // 三维距离**不可能**滤掉，跟站位无关，是结构性偏移。
                    // 阈值 1.5 米：手电筒实测水平只差 0.36 米，但 1.0 米实战仍有漏网，
                    // 说明别的挂件(火箭/橄榄球等)挂点更靠外。代价是贴身队友手里的道具也会被隐藏。
                    float prop_hdist = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2)) / dist_scale;
                    draw_prop = prop_hdist >= 1.5f;
                }
                if (draw_prop) {
                    const char* propName = getprop(data[i].class_name);
                    if (propName) {
                        std::string s = propName;
                        if (!on_body) {
                            if (g_english) s += " ";      // 中文名后直接接数字，英文要隔开
                            s += std::to_string((int) dist);
                            s += T(" 米", " m");
                        }
                        auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                        Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
                    }
                }
            }

            if (!ShouldSkipEntity(data[i])){
                if (show_draw_Role&&strstr(data[i].class_name, "chr") != NULL){
                    sprintf(objtext, "%lx", data[i].obj);   // objtext 原来只在调试分支里写，这里会显示别的对象的地址
                    std::string test;
                    test += " [";
                    test += std::to_string((int) dist);
                    test += T(" 米]  0x", " m]  0x");
                    test += objtext;
                    test += T(" [类名] ", " [class] ");
                    test += data[i].class_name;
                    auto textSize = ImGui::CalcTextSize(test.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), test.c_str());
                }
                			            
                std::string s;

                // 离自己 1 米以内的角色不画方框和附带数据(名字/距离/天赋/特质/射线)：
                // 贴身的基本是自己身上的其它模型(备用形态、残留模型)，画出来只会挡视线。
                // 角色原点都在脚底，三维距离即可(道具那边是挂点偏高，才只能算水平距离)。
                // 没有自身锚点时 Z 不可信，不做这层过滤。
                bool too_close = (self_obj != 0) && dist < 1.0f;

                if (!too_close && (data[i].camp==1||data[i].camp==2)){
                    if (g_diag_box_n < 64) g_diag_box[g_diag_box_n++] = data[i].obj;   // 【临时诊断】
                    // 本帧画了骨骼就用骨骼包围盒当方框(名字/距离/天赋/射线都跟着它走)，没骨骼才用上面写死高度那套
                    {
                        float bx1, by1, bx2, by2;
                        if (bone_box_get(data[i].obj, bx1, by1, bx2, by2)) {
                            X1 = bx1; Y1 = by1; X2 = bx2; Y2 = by2; W = X2 - X1; H = Y2 - Y1;
                        }
                    }
                    // 模仿者模式：名字换成"N号 身份"，文字和方框按阵营着色(侦探团蓝/模仿者红/中立黄)。
                    // 身份进局约 2 秒后才赋值，之前只有号码、画白色；不是模仿者玩家本体的对象照旧画角色名
                    int cc_identity = 0, cc_seat = 0;
                    bool cc = copycat_mode && PySelf::copycat_identity(data[i].obj, cc_identity, cc_seat);
                    // 约瑟夫镜世界(按身份固定，不再按我所在世界翻转)：
                    //   echo=镜像投影(白框，静止死物)；真人=绿/红框 + 名字后缀标它自己在哪个世界
                    bool joseph = PySelf::is_joseph_match();
                    bool echo   = PySelf::body_is_mirror(data[i].obj);
                    ImColor name_color = ImColor(255,200,0,255);
                    if (cc){
                        char cc_text[48];
                        copycat_label(cc_identity, cc_seat, cc_text, sizeof(cc_text));
                        s += cc_text;
                        name_color = copycat_color(cc_identity);
                    } else
                    {
                        s+=data[i].str;
                    }
                    // 约瑟夫局：真人名字后缀标它在哪个世界(在镜世界=镜像/否则=现实)；投影(echo)不加后缀
                    if (joseph && !echo)
                        s += PySelf::body_in_photo_world(data[i].obj) ? T(" 镜像", " Mirror") : T(" 现实", " Real");
                    if (echo) name_color = BotBoneColor;   // 镜像投影：名字白
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({X1 + W/2-(textSize.x/2),Y1-45}, name_color, s.c_str());
                    // 约瑟夫局：方框永远画(无视"绘制方框"开关)。镜像投影白 / 真实监管红 / 真实求生者绿。
                    // 非约瑟夫局(场上无 PhotoUnit，不会有投影)走原来的开关逻辑。
                    if (joseph){
                        ImColor bc = echo ? BotBoneColor
                                   : (cc ? name_color
                                   : (data[i].camp==1 ? BoneColor : color_green));
                        corner_box({X1, Y1},{X2, Y2}, bc,3, 0,1.8f);
                    }
                    else if (show_draw_Rect){
        			    if (cc)
        			        corner_box({X1, Y1},{X2, Y2}, name_color,3, 0,1.8f);
        			    else if (IsGhostEntity(data[i]))
        			        corner_box({X1, Y1},{X2, Y2}, BotBoneColor,3, 0,1.8);
        			    else if (data[i].camp==1)
        			        corner_box({X1, Y1},{X2, Y2}, BoneColor,3, 0,1.8f);
        			    else if (data[i].camp==2)
        			        corner_box({X1, Y1},{X2, Y2}, color_green,3, 0,1.8f);
        			}
                    float next_y = Y2 + 10;            // 距离、天赋、辅助特质依次往下排
                    const float line_h = ImGui::GetFontSize() + 2.0f;
                    if (show_draw_Distance || echo){   // 镜像投影默认画距离(白框+名字+距离固定集)
                        std::string dist_str;
                        dist_str += std::to_string((int) dist);
                        dist_str += T(" 米", " m");
                        auto textSize = ImGui::CalcTextSize(dist_str.c_str(), 0, 25);
                        Draw->AddText({X1 + W/2-(textSize.x/2),next_y}, ImColor(255,200,0,255), dist_str.c_str());
                        next_y += line_h;
                    }

                    // 天赋简称：求生者大心脏排最后、监管挽留排最后，格式化在 PyGenius.h 里
                    // 约瑟夫镜世界：另一个世界的人(定格死物/对面世界)不画天赋，避免重复刷一堆
                    if (show_draw_Genius && !echo){   // 镜像投影不画天赋(真人照常)
                        PyGenius::Info gi;
                        if (PyGenius::lookup(data[i].obj, D.X, D.Y, gi)){
                            PyGenius::GeniusLine seg;
                            PyGenius::genius_segments(gi, seg);
                            if (seg.pre[0] || seg.flywheel[0] || seg.post[0]){
                                // 绝处逢生三态：没带=白 / 带了还没用=绿 / 带了已经用掉=灰。
                                // 消耗标志是 unit.ability_used[102]，2026-09-22 实测**别人的也读得到**
                                // (服务器会下发非本机玩家的消耗状态)，所以监管看四个人都准。
                                ImColor c;
                                switch (PyGenius::desperate_state(gi)) {
                                    case PyGenius::DESPERATE_AVAILABLE: c = color_green; break;
                                    case PyGenius::DESPERATE_USED: c = ImColor(140,140,140,255); break;
                                    // DESPERATE_NONE(表完整且没带) 和 DESPERATE_UNKNOWN(表还没读全) 都画白色；
                                    // 后者的文本末尾带 "?"，靠它区分
                                    case PyGenius::DESPERATE_UNKNOWN:
                                    default:              c = ImColor(255,255,255,255); break;
                                }
                                // 飞轮就绪时"飞轮"两个字画红，优先于上面的整行配色
                                ImColor flywheel_color = seg.flywheel_ready ? color_red : c;
                                // 求生者的天赋行小一号(约 85%，2026-11-05 用户要求)；监管那行照旧
                                const float gfs = data[i].camp == 2 ? ImGui::GetFontSize() * 0.85f : ImGui::GetFontSize();
                                float w_pre = fi_calc(gfs, seg.pre).x;
                                float w_flywheel = fi_calc(gfs, seg.flywheel).x;
                                float w_post = fi_calc(gfs, seg.post).x;
                                float tx = X1 + W/2 - (w_pre + w_flywheel + w_post)/2;
                                if (seg.pre[0])   fi_text(Draw, tx, next_y, gfs, c, seg.pre);
                                if (seg.flywheel[0]) fi_text(Draw, tx + w_pre, next_y, gfs, flywheel_color, seg.flywheel);
                                if (seg.post[0])   fi_text(Draw, tx + w_pre + w_flywheel, next_y, gfs, c, seg.post);
                                next_y += gfs + 2.0f;
                            }
                            // 监管再单独一行写当前辅助特质 + 剩余冷却
                            // (带底牌会局中换特质，所以读的是实时值；冷却来自 skill_mgr，见 PyGenius.h)
                            // 梦之信徒(236)只有这一行：每个信徒有自己独立的闪现等特质冷却
                            if ((gi.camp == 1 || gi.camp == PyGenius::YIDHRA_PUPPET_UNIT_TYPE) && gi.support_trait != 0){
                                char trait_line[48];
                                PyGenius::trait_line_text(gi, PyGenius::trait_name(gi.support_trait), trait_line, sizeof(trait_line));
                                if (trait_line[0]){
                                    // 就绪=绿，冷却中=白；读不到冷却时按白显示(只有名字)
                                    ImColor cc = PyGenius::cd_ready(gi) ? color_green : ImColor(255,255,255,255);
                                    auto ts2 = ImGui::CalcTextSize(trait_line, 0, 25);
                                    Draw->AddText({X1 + W/2-(ts2.x/2),next_y}, cc, trait_line);
                                    next_y += line_h;
                                }
                            }
                        }
                    }

                    if (show_draw_Line && !copycat_mode && !echo){   // 模仿者模式、约瑟夫镜像投影不画射线(用户要求)
                        ImGui::GetForegroundDrawList()->AddLine({px, 160},{X1 + W/2, Y1}, ImColor(255, 255, 255),2);
                    }
                }
            }                                                          			
    	}//判断w    
	     
	                
	    if (mirror&&redqueenmod) draw_mirror_image(i);   // 红夫人镜像(见循环前的 draw_mirror_image)
    }

    // ---- 红夫人镜像(镜中红夫人 MaryMirrorImageUnit，键 247)：我是求生者时画，不看「夫人模式」(2026-11-05 用户要求) ----
    // 它的类名跟本体一样是 redqueen.gim、归监管，但不在本体集合([1]/[2]/[236]/[1065])里，原来被本体过滤挡掉，一直没画过。
    // 镜子没放出时它停在地下(y≈-1000)，高度 <= -300 不画。方框/距离跟着各自的开关
    float mi_c[3] = {0, 0, 0};
    uint64_t mi_coor = mirror_img ? getPtr64(mirror_img + 0x28) : 0;
    bool mi_read = mi_coor && vm_readv(mi_coor + 0xa0, mi_c, 12);
    {   // 【临时诊断】镜中红夫人：对象换了 / 升到地面或沉回地下时打一行
        static uint64_t last_mi = 0; static int last_up = -1;
        int up = (mi_read && mi_c[1] > -300) ? 1 : 0;
        if (mirror_img != last_mi || up != last_up)
            diag_log("[框诊断] 镜中红夫人 obj=0x%llx 来源=%s 坐标=(%.1f,%.1f,%.1f) 在地面=%d 我的阵营=%d 有锚点=%d\n",
                     (unsigned long long)mirror_img, PyProgress::mirror_image_obj() ? "Python247" : (mirror_img ? "场景+0x6D" : "无"),
                     mi_c[0], mi_c[1], mi_c[2], up, (int)self_camp, (int)has_auth_self);
        last_mi = mirror_img; last_up = up;
    }
    if (mi_read && mi_c[0] != 0 && mi_c[2] != 0 && mi_c[1] > -300
        && has_auth_self && self_camp == 2) {
        float qx = mi_c[0], qh = mi_c[1], qy = mi_c[2];
        float cam = matrix[3]*qx + matrix[7]*qh + matrix[11]*qy + matrix[15];
        float d = sqrt(pow(qx - Z.X, 2) + pow(qy - Z.Y, 2) + pow(qh - Z.Z, 2)) / dist_scale;
        if (cam > 0.01f && d < 300) {
            float cx = px + (matrix[0]*qx + matrix[4]*qh + matrix[8]*qy + matrix[12]) / cam * px;
            float cy = py - (matrix[1]*qx + matrix[5]*(qh + 8.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float cw = py - (matrix[1]*qx + matrix[5]*(qh + 28.5f) + matrix[9]*qy + matrix[13]) / cam * py;
            float bh = cy - cw, bw = bh / 2;
            if (bw > 0) {
                float bx1 = cx - bh / 4, by1 = cy - bh / 2;
                {   // 方框按它自己的骨骼算(跟普通人物一致)，骨骼读不到才用上面写死高度那套
                    NativeBones::Skeleton msk;
                    float sx1, sy1, sx2, sy2;
                    if (NativeBones::read_unit(mirror_img, false, msk) && msk.valid && skeleton_screen_box(msk, false, sx1, sy1, sx2, sy2)) {
                        bx1 = sx1; by1 = sy1; bw = sx2 - sx1; bh = sy2 - sy1;
                    }
                }
                const char *nm = T("红夫人镜像", "Queen's Mirror Image");
                auto ts = ImGui::CalcTextSize(nm, 0, 25);
                Draw->AddText({bx1 + bw/2 - ts.x/2, by1 - 45}, ImColor(255,200,0,255), nm);
                if (show_draw_Rect) corner_box({bx1, by1}, {bx1 + bw, by1 + bh}, BoneColor, 3, 0, 1.8f);
                if (show_draw_Distance) {
                    char dt[24];
                    snprintf(dt, sizeof(dt), T("%d 米", "%d m"), (int)d);
                    auto ds = ImGui::CalcTextSize(dt, 0, 25);
                    Draw->AddText({bx1 + bw/2 - ds.x/2, by1 + bh + 10}, ImColor(255,200,0,255), dt);
                }
            }
        }
    }

    // ---- 【临时诊断】骨骼画了、方框那段却没走到的本体 ----
    // 原因按主循环的过滤顺序重查一遍(-1 = 设计上就不画框，不记)。一个"缺框片段"结束时打一行：
    // 持续帧数 / 开始那帧的原因 / 片段内有没有撞上读线程改写 data[](撕裂窗口) / objcoor 跟 [M+0x28] 对不对得上
    {
        static uint64_t frame_no = 0;
        frame_no++;
        const bool tear = diag_busy0 || g_scan_busy.load() || diag_gen0 != g_scan_gen.load();
        auto reason_of = [&](uint64_t M, int &idx, int &extra, bool &coor_bad) -> int {
            idx = -1; extra = 0; coor_bad = false;
            for (int i = 0; i < data_count; i++) if (data[i].obj == M) { idx = i; break; }
            if (idx < 0) { extra = data_count; return 1; }                                   // 不在 data[]
            const DataStruct &e = data[idx];
            uint64_t fresh = getPtr64(M + 0x28);
            coor_bad = fresh != (uint64_t)e.objcoor;
            if (strstr(e.class_name, "buzz") || strstr(e.class_name, "nvyao.gim")) return 12;
            if (self_obj != 0 && M == self_obj) return -1;
            if ((e.camp == 1 || e.camp == 2) && PySelf::in_lobby()) return 3;
            if (copycat_mode) return -1;
            if ((e.camp == 1 || e.camp == 2) && PySelf::body_set_ready()
                && (strstr(e.class_name, "player") || strstr(e.class_name, "boss")) && !PySelf::is_body(M)) return 5;
            float c[3] = {0, 0, 0};
            vm_readv(e.objcoor + 0xa0, c, 12);
            if (c[0] == 0 || c[2] == 0) return 7;
            if (c[1] <= -300) { extra = 1; return 7; }
            if (has_auth_self && self_camp == 1 && e.camp == 1 && !PySelf::is_teammate_butcher(M)) return -1;
            if (has_auth_self && self_camp == 1 && e.camp == 2 && PySelf::is_carried(M)) return -1;
            float cam = matrix[3]*c[0] + matrix[7]*c[1] + matrix[11]*c[2] + matrix[15];
            float d = sqrt(pow(c[0]-Z.X,2) + pow(c[2]-Z.Y,2) + pow(c[1]-Z.Z,2)) / dist_scale;
            if (d >= 300) return 9;
            float ry = py - (matrix[1]*c[0] + matrix[5]*(c[1]+8.5f) + matrix[9]*c[2] + matrix[13]) / cam * py;
            float rw = py - (matrix[1]*c[0] + matrix[5]*(c[1]+28.5f) + matrix[9]*c[2] + matrix[13]) / cam * py;
            if ((ry - rw) / 2 <= 0) return 8;
            if (ShouldSkipEntity(e)) { extra = PySelf::is_stale_model(M) ? 1 : (IsGhostEntity(e) ? 2 : 3); return 4; }
            if (self_obj != 0 && d < 1.0f) return 10;
            if (e.camp != 1 && e.camp != 2) { extra = e.camp; return 2; }
            return 0;                                                                        // 全部过滤都过得去
        };
        static const char *const RN[] = {"过滤都通过(本帧读到的 data[] 跟循环时不一样?)", "不在data[]", "camp不是1/2",
                                         "判成局外(in_lobby)", "ShouldSkip(1废弃模型/2幽灵/3类名黑名单)", "不在本体集合",
                                         "", "坐标为0或在地下(extra=1地下)", "W<=0(相机背后)", "距离>=300", "离自己<1m",
                                         "", "类名buzz/nvyao"};
        struct Ep { uint64_t start; int frames; int reason; int extra; int idx; bool tear; bool coor_bad; int count0; };
        static std::unordered_map<uint64_t, Ep> eps;
        uint64_t miss[32]; int nmiss = 0;
        for (int u = 0; u < g_diag_bone_n; u++) {
            uint64_t M = g_diag_bone[u];
            bool boxed = false;
            for (int k = 0; k < g_diag_box_n; k++) if (g_diag_box[k] == M) { boxed = true; break; }
            if (boxed) continue;
            int idx, extra; bool coor_bad;
            int r = reason_of(M, idx, extra, coor_bad);
            if (r < 0) continue;
            miss[nmiss++] = M;
            auto it = eps.find(M);
            if (it == eps.end()) {
                eps[M] = {frame_no, 1, r, extra, idx, tear, coor_bad, diag_count0};
                if (r == 7) {   // 位置字段说在地下/原点，骨骼却画出来了：把两份坐标都打出来，看哪份过期
                    uint64_t oc = getPtr64(M + 0x28);
                    float pos[3] = {0, 0, 0}, wm[16] = {0};
                    if (oc) { vm_readv(oc + 0xa0, pos, 12); vm_readv(oc + 0x1C0, wm, 64); }
                    diag_log("[框诊断] 缺框开始 本体0x%llx 类名=%s 位置+0xa0=(%.1f,%.1f,%.1f) 世界矩阵平移=(%.1f,%.1f,%.1f) 自身=(%.1f,%.1f,%.1f)\n",
                             (unsigned long long)M, idx >= 0 ? data[idx].class_name : "?", pos[0], pos[1], pos[2],
                             wm[12], wm[13], wm[14], Z.X, Z.Z, Z.Y);
                }
            }
            else {
                Ep &ep = it->second;
                ep.frames++; ep.tear |= tear; ep.coor_bad |= coor_bad;
                if (ep.frames == 120)
                    diag_log("[框诊断] 缺框持续中 本体0x%llx 已 %d 帧 原因=%s extra=%d idx=%d objcoor不一致=%d data_count=%d\n",
                             (unsigned long long)M, ep.frames, RN[ep.reason], ep.extra, ep.idx, (int)ep.coor_bad, data_count);
            }
        }
        for (auto it = eps.begin(); it != eps.end();) {
            bool still = false;
            for (int k = 0; k < nmiss; k++) if (miss[k] == it->first) { still = true; break; }
            if (still) { ++it; continue; }
            bool bones_now = false;
            for (int u = 0; u < g_diag_bone_n; u++) if (g_diag_bone[u] == it->first) { bones_now = true; break; }
            const Ep &ep = it->second;
            diag_log("[框诊断] 缺框 本体0x%llx 持续 %d 帧 原因=%s extra=%d idx=%d 撕裂窗口=%d objcoor不一致=%d data_count(开始)=%d 结束方式=%s\n",
                     (unsigned long long)it->first, ep.frames, RN[ep.reason], ep.extra, ep.idx, (int)ep.tear, (int)ep.coor_bad,
                     ep.count0, bones_now ? "框回来了" : "骨骼也不画了");
            it = eps.erase(it);
        }
    }
    // 发布版本: 注入功能已停用
    // if (show_sohook)
    //     SoHook::DrawOverlay(Draw, matrix, px, py, 内核人物数量,
    //                         Z.X, Z.Z, Z.Y, 距离比例);
}
        

// 内存占用（KB）。原来只读 statm 的 RSS，漏了两大块：换进 zram 的页、图形缓冲。
// 2026-09-24 实测 RSS 22MB，而实际约 216MB：交换链 4 张 3200x3200 占 157MB（dmabuf）、
// GPU 私有显存 20MB（字体纹理 16MB）、换出 33MB，RSS 里 18MB 还是共享库。
struct MemUsage {
    size_t pss_kb = 0;      // 进程自身常驻（共享页按进程数分摊）
    size_t swap_kb = 0;     // 已换进 zram
    size_t gpu_kb = 0;      // GPU 私有显存（仅 kgsl 能读到）
    size_t buffer_kb = 0;   // 画面缓冲（dmabuf，交换链图像）
    bool gpu_ok = false;
    size_t total_kb() const { return pss_kb + swap_kb + gpu_kb + buffer_kb; }
};

static size_t read_size_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) != 1) v = 0;
    fclose(f);
    return (size_t)v;
}

static MemUsage read_memory_usage() {
    MemUsage m;
    if (FILE *f = fopen("/proc/self/smaps_rollup", "r")) {
        char line[128];
        size_t v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "Pss: %zu kB", &v) == 1) m.pss_kb = v;
            else if (sscanf(line, "SwapPss: %zu kB", &v) == 1) m.swap_kb = v;
        }
        fclose(f);
    }

    // 高通：kgsl 按进程统计，kernel = GPU 私有、imported_mem = 导入的 dmabuf，单位字节
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/kgsl/kgsl/proc/%d/kernel", getpid());
    if (access(path, R_OK) == 0) {
        m.gpu_ok = true;
        m.gpu_kb = read_size_file(path) / 1024;
        snprintf(path, sizeof(path), "/sys/class/kgsl/kgsl/proc/%d/imported_mem", getpid());
        m.buffer_kb = read_size_file(path) / 1024;
        return m;
    }

    // 非高通：只能从 fdinfo 统计持有的 dmabuf，GPU 私有显存读不到
    if (DIR *dir = opendir("/proc/self/fd")) {
        while (dirent *e = readdir(dir)) {
            if (e->d_name[0] == '.') continue;
            char link[128], target[128];
            snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (!strstr(target, "dmabuf")) continue;
            snprintf(link, sizeof(link), "/proc/self/fdinfo/%s", e->d_name);
            if (FILE *f = fopen(link, "r")) {
                char line[128];
                size_t v;
                while (fgets(line, sizeof(line), f))
                    if (sscanf(line, "size: %zu", &v) == 1) { m.buffer_kb += v / 1024; break; }
                fclose(f);
            }
        }
        closedir(dir);
    }
    return m;
}

int GetInputDeviceCount() {
    DIR *dir = opendir("/dev/input/");
    if (!dir) return -1;
    dirent *ptr = NULL;
    int count = 0;
    while ((ptr = readdir(dir)) != NULL) {
        if (strstr(ptr->d_name, "event"))
            count++;
    }
    closedir(dir);
    return count ? count : -1;
}

void VolumeKeyHide() {
    int EventCount = GetInputDeviceCount();
    if (EventCount <= 0) return;

    int *fdArray = (int *)malloc(EventCount * sizeof(int));
    if (!fdArray) return;

    for (int i = 0; i < EventCount; i++) {
        char temp[128];
        sprintf(temp, "/dev/input/event%d", i);
        fdArray[i] = open(temp, O_RDONLY | O_NONBLOCK);   // 只读按键，不需要写权限
    }

    input_event ev;
    while (1) {
        for (int i = 0; i < EventCount; i++) {
            if (fdArray[i] < 0) continue;
            memset(&ev, 0, sizeof(ev));
            while (read(fdArray[i], &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEDOWN && ev.value == 1)
                    voice = false;
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEUP && ev.value == 1)
                    voice = true;
            }
        }
        show_window = voice;
        usleep(10000);
    }
    free(fdArray);
}

#include <sys/stat.h>
#include <ctime>
// ---- 截图(带叠加层)：手机自带截图按图层/UID 过滤，截不到叠加层；root 跑 screencap 抓的是整屏合成结果，叠加层在里面 ----
// 流程：点按钮 -> 主菜单隐藏 3 帧(让不带菜单的画面先显示出来) -> 后台线程 screencap 存到相册截图目录 -> 通知媒体库 -> 菜单恢复
static std::atomic<int> g_shot_state{0};      // 0 空闲  1 正在藏菜单(等帧)  2 正在截
static int  g_shot_wait_frames = 0;
static char g_shot_msg[200] = "";
static std::atomic<bool> g_shot_msg_new{false};

static void shot_worker()
{
    time_t now = time(nullptr);
    struct tm lt; localtime_r(&now, &lt);
    char path[160];
    strftime(path, sizeof(path), "/storage/emulated/0/Pictures/Screenshots/IdentityV_%Y%m%d_%H%M%S.png", &lt);
    system("mkdir -p /storage/emulated/0/Pictures/Screenshots");
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "screencap -p '%s'", path);
    int rc = system(cmd);
    struct stat st;
    if (rc == 0 && stat(path, &st) == 0 && st.st_size > 0) {
        snprintf(cmd, sizeof(cmd), "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://%s' >/dev/null 2>&1", path);
        system(cmd);                                              // 让相册立刻看到
        snprintf(g_shot_msg, sizeof(g_shot_msg), T("已截图: %s", "Screenshot saved -> %s"), path + strlen("/storage/emulated/0/"));
    } else {
        snprintf(g_shot_msg, sizeof(g_shot_msg), "%s", T("截图失败(screencap 没有输出)", "Screenshot failed (screencap produced nothing)"));
    }
    g_shot_msg_new = true;
    g_shot_state = 0;
}

static void shot_tick()
{
    if (g_shot_msg_new.exchange(false)) cfg_set_msg(g_shot_msg);
    if (g_shot_state == 1 && --g_shot_wait_frames <= 0) {
        g_shot_state = 2;
        std::thread(shot_worker).detach();
    }
}

// 选项后面的灰色 (?)：手指按住显示说明(触屏松手后提示会留着，点别处消失)
static void help_marker(const char *zh, const char *en)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 18.0f);
        ImGui::TextUnformatted(T(zh, en));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Layout_tick_UI(bool *main_thread_flag) {
    // 启动后第一帧：先记默认值，再读 /data 下的配置(要在 ImGui 上下文建好之后，cfg_set_msg 用到 GetTime)
    static bool cfg_autoloaded = false;
    if (!cfg_autoloaded) {
        cfg_autoloaded = true;
        config_snapshot_defaults();
        cfg_mark_saved();          // 没有配置文件时以默认值为基准；读到文件时 config_load 里会再记一次
        config_load(true);
    }

    static bool volume_thread_started = false;
    if (!volume_thread_started) {
        std::thread(VolumeKeyHide).detach();
        volume_thread_started = true;
    }

    px = static_cast<float>(displayInfo.width) / 2;
    py = static_cast<float>(displayInfo.height) / 2;
    // 发布版本: 注入功能已停用
    // SoHook::Update(pid);

    Draw_Main(ImGui::GetForegroundDrawList());
    shot_tick();

    if (show_window && (g_shot_state == 0 || !shot_hide_menu)) {  // 截图时按开关决定藏不藏主菜单

        ImGui::SetNextWindowBgAlpha(panel_alpha);
        if (cfg_apply_main) { ImGui::SetNextWindowPos({cfg_main_x, cfg_main_y}, ImGuiCond_Always); cfg_apply_main = false; }
        ImGui::Begin("New_Edition", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        cfg_main_x = ImGui::GetWindowPos().x; cfg_main_y = ImGui::GetWindowPos().y;   // 保存配置时用

        if (::permeate_record_ini) {
            ImGui::SetWindowPos({LastCoordinate.Pos_x, LastCoordinate.Pos_y});
            ImGui::SetWindowSize({LastCoordinate.Size_x, LastCoordinate.Size_y});
            permeate_record_ini = false;
        }

        // 状态框：只放每次都要看的(数据状态/帧率/内存总数)和常用按钮；渲染模式、内存明细在「基础信息」里
        ImGui::BeginChild("##status_box", ImVec2(0, 0),
                          ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);

        // 读 smaps_rollup 要遍历页表，没必要每帧读，1 秒刷新一次
        static MemUsage mem;
        static double mem_time = -10.0;
        if (ImGui::GetTime() - mem_time >= 1.0) {
            mem = read_memory_usage();
            mem_time = ImGui::GetTime();
        }

        // read_state: 0 = 还在 bss 里扫签名(此时什么都不画)，1 = 数组失效重试中，2 = 正常
        if (read_state == 2)
            ImGui::TextColored(ImVec4(0.0f, 0.6f, 0.0f, 1.0f), "%s", T("已获取游戏数据", "Game data acquired"));
        else if (read_state == 1)
            ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.2f, 1.0f), "%s", T("正在获取游戏数据", "Acquiring game data"));
        else
            ImGui::TextDisabled("%s", T("正在定位游戏数据(扫描签名)", "Locating game data (scanning)"));
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImTextT("%.0f FPS  内存 %.0f MB", "%.0f FPS  Mem %.0f MB", ImGui::GetIO().Framerate, mem.total_kb() / 1024.0f);

        // 语言切换：点击在 中文/英文 间切换(g_english 定义在 Name.h)。
        // 按钮文字显示「点击后会切到的语言」：当前中文时显示 English,当前英文时显示 中文。
        // pick() 实时读 g_english,缓存存的是映射项指针,所以切换即时生效、无需清缓存。
        if (ImGui::Button(g_english ? "中文" : "English"))
            g_english = !g_english;
        ImGui::SameLine();
        if (ImGui::Button(T("截图(含叠加层)", "Screenshot (with overlay)")) && g_shot_state == 0) {
            g_shot_wait_frames = shot_hide_menu ? 3 : 0;      // 藏菜单要先等几帧让不带菜单的画面显示出来
            g_shot_state = 1;
        }
        ImGui::SameLine();
        ImGui::Checkbox(T("截图时隐藏菜单", "Hide menu in shot"), &shot_hide_menu);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::SliderFloat(T("透明度###panel_alpha", "Opacity###panel_alpha"), &panel_alpha, 0.10f, 1.0f, "%.2f");
        // 有改动还没写进文件时按钮带 *(### 后面是固定 ID，文字变了也是同一个按钮)
        bool dirty = cfg_dirty();
        if (ImGui::Button(dirty ? T("保存配置 *###cfg_save", "Save Config *###cfg_save")
                                : T("保存配置###cfg_save", "Save Config###cfg_save"))) config_save();
        ImGui::SameLine();
        if (ImGui::Button(T("恢复默认配置###cfg_reset", "Reset to Defaults###cfg_reset"))) config_restore_defaults();
        if (cfg_msg[0] && ImGui::GetTime() - cfg_msg_time < 4.0)   // 结果提示停留 4 秒
            ImGui::TextColored(strstr(cfg_msg, "失败") || strstr(cfg_msg, "failed") ? ImVec4(1, 0.35f, 0.35f, 1) : ImVec4(0.4f, 1, 0.4f, 1), "%s", cfg_msg);

        // 结束进程：单独放最后一行、红色，点两次才退出(第一次点后 3 秒内再点一次)，防触屏误触
        static double exit_armed_at = -100.0;
        bool exit_armed = ImGui::GetTime() - exit_armed_at < 3.0;
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        exit_armed ? ImVec4(0.85f, 0.15f, 0.15f, 1) : ImVec4(0.75f, 0.30f, 0.30f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.20f, 0.20f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f, 0.10f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1, 1, 1, 1));
        if (ImGui::Button(exit_armed ? T("再点一次确认退出###exit", "Tap again to exit###exit")
                                     : T("结束进程###exit", "Exit###exit"))) {
            if (exit_armed) exit(0);
            exit_armed_at = ImGui::GetTime();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndChild();

        if (ImGui::CollapsingHeader(T("基础信息###base", "Basic Info###base"))) {
            ImTextT("渲染模式 : %s, gui版本 : %s", "Render: %s  gui: %s", graphics->RenderName, IMGUI_VERSION);
            if (mem.gpu_ok)
                ImTextT("内存 %.1f MB = 进程 %.1f + 换出 %.1f + 显存 %.1f + 画面缓冲 %.1f",
                        "Mem %.1f MB = proc %.1f + swap %.1f + vram %.1f + surface %.1f",
                        mem.total_kb() / 1024.0f, mem.pss_kb / 1024.0f, mem.swap_kb / 1024.0f, mem.gpu_kb / 1024.0f, mem.buffer_kb / 1024.0f);
            else
                ImTextT("内存 %.1f MB = 进程 %.1f + 换出 %.1f + 画面缓冲 %.1f (显存未计)",
                        "Mem %.1f MB = proc %.1f + swap %.1f + surface %.1f (vram n/a)",
                        mem.total_kb() / 1024.0f, mem.pss_kb / 1024.0f, mem.swap_kb / 1024.0f, mem.buffer_kb / 1024.0f);
            ImGui::Separator();
            ImTextT("游戏进程:%d", "PID:%d", pid);
            ImTextT("模块入口:%lx", "Module base:%lx", libbase);
            ImTextT("游戏包名:%s", "Package:%s", extractedString);
            ImTextT("矩阵地址:%lx", "Matrix:%lx", Matrix);
            ImTextT("数组地址:%lx", "Array:%lx", Arrayaddr);
            ImTextT("矩阵偏移:%lx", "Matrix off:%lx", MatrixOffset);
            ImTextT("数组偏移:%lx", "Array off:%lx", ArrayaddrOffset);
            ImTextT("监管者:%s (房间监管%d)", "Hunter:%s (room hunters %d)", prophet_text, g_room_hunter_n);
            ImTextT("Py根:%s", "PyRoot:%s", PyRoot::status_text());
            ImTextT("密码机进度:%s (已破译%d)", "Ciphers:%s (decoded %d)", PyProgress::status_text(), PyProgress::decoded_count());
            ImTextT("天赋:%s", "Talents:%s", PyGenius::status_text());
            ImTextT("阶段:%s", "Phase:%s", PyScene::status_text());
            // 自身锚点：链路一旦失效这里会写明原因；拿不到锚点时没有自身(不猜)
            ImTextT("自身:%s", "Self:%s", PySelf::status_text());
            ImTextT("镜世界:%s", "Mirror:%s", PySelf::photo_diag_text());   // 约瑟夫 PhotoUnit 激活状态(_time/_players)
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(T("绘制设置###draw", "Drawing###draw"))) {
            // 两列表格：复选框按列对齐(SameLine 排的话标签长短不一，第二列对不齐)
            if (ImGui::BeginTable("##draw_tbl", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示鬼魂", "Show Ghosts"), &inform_ghost);
                help_marker("鬼魂视角/幽灵态的角色也画出来(白框)", "Also draw characters in ghost state (white box)");
                ImGui::TableNextColumn();
                // 预知监管和模仿者模式互斥(模仿者局没有监管)
                if (ImGui::Checkbox(T("预知监管", "Hunter Prophet"), &show_draw_prophet) && show_draw_prophet) { copycat_mode = false; waterfight_mode = false; }
                help_marker("屏幕顶部显示本局监管名字，准备阶段就能看到。打开会关掉模仿者/林中童话",
                            "Show this match's hunter at the top, already in the lobby. Turns off Copycat / Forest's Fable");

                ImGui::TableNextColumn();
                ImGui::Checkbox(T("绘制道具", "Draw Items"), &show_draw_Prop);
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("夫人模式", "Queen Mode"), &redqueenmod);
                help_marker("红夫人瞄准或放下镜子时，画出求生者在镜中的位置(方框+骨骼)",
                            "When Bloody Queen aims or places a mirror, draw survivors' mirror images (box + bones)");

                ImGui::TableNextColumn();
                ImGui::Checkbox(T("绘制调试", "Draw Debug"), &Debugging);
                help_marker("给每个场景对象标地址和类名，排查用，画面会很乱", "Label every scene object with address and class name (for troubleshooting, very noisy)");
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示密码机", "Show Ciphers"), &show_draw_secret_mechine);   // 局内进度/进度条 跟着它

                // 「显示天赋」「显示大门」已移到「固定信息」折叠框(用户要求)

                // 发布版本: 注入功能已停用
                // ImGui::Checkbox("骨骼与进度", &show_sohook);
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示骨骼", "Show Bones"), &show_draw_Bones);   // 原生骨骼,抄若辰链驱动读
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示方框", "Show Box"), &show_draw_Rect);

                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示射线", "Show Lines"), &show_draw_Line);   // 默认开(变量初值 true)
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("监管天赋树", "Hunter Talent Tree"), &show_genius_tree);   // 径向图，独立可拖动窗口
                help_marker("单独的窗口，显示监管点了哪些天赋", "Separate window showing the hunter's talent picks");
                ImGui::EndTable();
            }
        }

        // 准星：所有角色通用，默认关。位置/样式/大小/粗细整套按角色记(保存配置时写进去)；
        // 位置同时是蜡像师瞄准线对齐用的标记位置。没设过的角色沿用蜡像师的，再没有用默认(游戏蜡枪标记，十字+点)
        if (ImGui::CollapsingHeader(T("准星###xhair", "Crosshair###xhair"))) {
            float mw = (float)displayInfo.width, mh = (float)displayInfo.height;
            if (WaxAim::aim_x < 0) WaxAim::aim_x = mw * 0.5f;
            if (WaxAim::aim_y < 0) WaxAim::aim_y = mh * WAX_MARK_Y;
            ImGui::Checkbox(T("显示准星##xh_on", "Show crosshair##xh_on"), &show_crosshair);
            ImGui::SameLine();
            if (ImGui::Button(T("恢复默认##xh_reset", "Reset##xh_reset"))) {
                // 删掉本角色的记录：回到蜡像师的(当前就是蜡像师则回到默认)
                if (g_xhair_pid > 0) g_xhair.erase(g_xhair_pid);
                if (g_xhair_pid == WaxAim::WAX_PID) { g_xh = XhairCfg(); g_xh.x = mw * 0.5f; g_xh.y = mh * WAX_MARK_Y; }
                else g_xh = xhair_for(g_xhair_pid);
                WaxAim::aim_x = g_xh.x; WaxAim::aim_y = g_xh.y;
            }
            bool changed = false;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::BeginCombo(T("样式##xh_style", "Style##xh_style"), xhair_style_name(g_xh.style))) {
                for (int s = 0; s < XH_STYLE_N; s++)
                    if (ImGui::Selectable(xhair_style_name(s), s == g_xh.style)) { g_xh.style = s; changed = true; }
                ImGui::EndCombo();
            }
            changed |= ImGui::SliderFloat(T("大小##xh_size", "Size##xh_size"), &g_xh.size, 3.0f, 60.0f, "%.0f");
            changed |= ImGui::SliderFloat(T("粗细##xh_thick", "Thickness##xh_thick"), &g_xh.thick, 1.0f, 8.0f, "%.1f");
            changed |= ImGui::SliderFloat(T("X##xh_x", "X##xh_x"), &WaxAim::aim_x, 0.0f, mw, "%.0f");
            changed |= ImGui::SliderFloat(T("Y##xh_y", "Y##xh_y"), &WaxAim::aim_y, 0.0f, mh, "%.0f");
            if (changed && g_xhair_pid > 0) {
                g_xh.x = WaxAim::aim_x; g_xh.y = WaxAim::aim_y;
                g_xhair[g_xhair_pid] = g_xh;
            }
            if (g_xhair_pid > 0)
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1), T("当前角色 pid %d：%s", "Role pid %d: %s"), g_xhair_pid,
                                   g_xhair.count(g_xhair_pid) ? T("用本角色的设置", "own settings") : T("沿用蜡像师/默认", "from Waxwork/default"));
        }

        // 固定信息(屏幕 HUD 汇总)：每块独立开关 + 独立 X/Y/大小。调好位置后把初值固化即可
        if (ImGui::CollapsingHeader(T("固定信息###fixed", "Fixed HUD###fixed"))) {
            float mw = (float)displayInfo.width, mh = (float)displayInfo.height;

            // 局内绘制的天赋行 / 大门：各自独立 bool，跟下面的固定信息面板互不共用
            ImGui::Checkbox(T("显示天赋", "Show Talents"), &show_draw_Genius);   // 天赋行 + 监管辅助特质行(局内画框那套)
            ImGui::SameLine();
            ImGui::Checkbox(T("显示大门", "Show Gates"), &show_draw_Door);
            ImGui::Separator();

            ImGui::Checkbox(T("密码机/门面板##fi_gen", "Cipher/Gate panel##fi_gen"), &fi_gen_on);
            if (fi_gen_on) {
                ImGui::SliderFloat(T("机 X##fi_gen", "Gen X##fi_gen"), &fi_gen_x, 0.0f, mw, "%.0f");
                ImGui::SliderFloat(T("机 Y##fi_gen", "Gen Y##fi_gen"), &fi_gen_y, 0.0f, mh, "%.0f");
                ImGui::SliderFloat(T("机 大小##fi_gen", "Gen size##fi_gen"), &fi_gen_size, 10.0f, 60.0f, "%.0f");
            }

            ImGui::Checkbox(T("监管特质/天赋##fi_hunter", "Hunter trait/talent##fi_hunter"), &fi_hunter_on);
            if (fi_hunter_on) {
                ImGui::SliderFloat(T("监 X(0=居中)##fi_hunter", "Htr X(0=center)##fi_hunter"), &fi_hunter_x, 0.0f, mw, "%.0f");
                ImGui::SliderFloat(T("监 Y##fi_hunter", "Htr Y##fi_hunter"), &fi_hunter_y, 0.0f, mh, "%.0f");
                ImGui::SliderFloat(T("监 大小##fi_hunter", "Htr size##fi_hunter"), &fi_hunter_size, 10.0f, 60.0f, "%.0f");
            }

            ImGui::Checkbox(T("求生者天赋##fi_civ", "Survivor talents##fi_civ"), &fi_civ_on);
            if (fi_civ_on) {
                ImGui::SliderFloat(T("求 X##fi_civ", "Svr X##fi_civ"), &fi_civ_x, 0.0f, mw, "%.0f");
                ImGui::SliderFloat(T("求 Y##fi_civ", "Svr Y##fi_civ"), &fi_civ_y, 0.0f, mh, "%.0f");
                ImGui::SliderFloat(T("求 大小##fi_civ", "Svr size##fi_civ"), &fi_civ_size, 10.0f, 60.0f, "%.0f");
                ImGui::SliderFloat(T("求 间距##fi_civ", "Svr gap##fi_civ"), &fi_civ_gap, 0.0f, 300.0f, "%.0f");
            }
        }


        // 蜡像师喷蜡辅助瞄准：把游戏的蜡枪瞄准标记抬进天空中的粗线里 = 能附着(见 WaxAim.h)
        if (ImGui::CollapsingHeader(T("蜡像师###wax", "Waxwork###wax"))) {
            const ImVec4 C_OK(0.4f, 1, 0.4f, 1), C_BAD(1, 0.45f, 0.45f, 1), C_DIM(0.65f, 0.65f, 0.65f, 1);
            ImGui::Checkbox(T("开启##wax_on", "Enable##wax_on"), &WaxAim::enabled);
            ImGui::SameLine();
            ImGui::Checkbox(T("落点小球##wax_traj", "Landing dot##wax_traj"), &WaxAim::show_traj);
            ImGui::Checkbox(T("不喷射时也画瞄准线##wax_always", "Aim lines when not spraying##wax_always"), &WaxAim::show_always);
            WaxAim::Status st = WaxAim::status();
            const char *cs = WaxAim::col_status();
            bool col_ok = strstr(cs, "三角") || strstr(cs, "tris");
            if (st.is_wax) ImGui::TextColored(col_ok ? C_OK : C_BAD, "%s", col_ok ? T("蜡像师：已识别，遮挡已加载", "Waxwork detected, occlusion loaded")
                                                                                   : T("蜡像师：已识别，本图没有遮挡文件", "Waxwork detected, no occlusion file for this map"));
            else           ImGui::TextColored(C_DIM, "%s", T("当前不是蜡像师", "Not playing Waxwork"));
        }

        // 模仿者模式：开关 + 身份簿。开着时名字换成"N号 身份"、不画射线；读到的身份记在簿子里，
        // 开会阶段页被换出读不到时沿用(见 PySelf.h 身份簿)。关掉开关簿子清空
        if (ImGui::CollapsingHeader(T("娱乐模式###fun", "Fun Modes###fun"))) {
            // 林中童话(打水仗 BR)：独立绘制玩家(动物阵营配色)+物资。开着自动关模仿者和预知监管
            if (ImGui::Checkbox(T("林中童话", "Forest's Fable"), &waterfight_mode) && waterfight_mode) { copycat_mode = false; show_draw_prophet = false; }
            if (waterfight_mode) ImGui::Text("%s", PyWaterFight::status_text());
            if (ImGui::Checkbox(T("模仿者", "Copycat"), &copycat_mode) && copycat_mode) { show_draw_prophet = false; waterfight_mode = false; }
            if (copycat_mode) {
                PySelf::CopycatBook book = PySelf::copycat_book();
                int known = 0, with_id = 0;
                for (int k = 0; k < PySelf::MAX_COPYCAT; k++)
                    if (book.seat[k].known) { known++; if (book.seat[k].identity) with_id++; }
                ImTextT("已记录 %d 人，身份已知 %d 人", "Recorded %d, identity known %d", known, with_id);
                for (int k = 0; k < PySelf::MAX_COPYCAT; k++) {
                    const PySelf::CopycatSeat &st = book.seat[k];
                    if (!st.known) continue;
                    const char *nm = PySelf::copycat_identity_name(st.identity);
                    char row[64];
                    int len = g_english ? snprintf(row, sizeof(row), "#%-2d ", k + 1)
                                        : snprintf(row, sizeof(row), "%2d号 ", k + 1);
                    if (nm) len += snprintf(row + len, sizeof(row) - len, "%s", nm);
                    else if (st.identity) len += g_english ? snprintf(row + len, sizeof(row) - len, "id%d", st.identity)
                                                           : snprintf(row + len, sizeof(row) - len, "身份%d", st.identity);
                    else len += snprintf(row + len, sizeof(row) - len, "?");
                    if (book.self_seat == k + 1) len += g_english ? snprintf(row + len, sizeof(row) - len, "  (me)")
                                                                  : snprintf(row + len, sizeof(row) - len, "  (我)");
                    if (st.dead) { if (g_english) snprintf(row + len, sizeof(row) - len, "  dead");
                                   else            snprintf(row + len, sizeof(row) - len, "  已死"); }
                    ImColor c = st.dead ? ImColor(140,140,140,255) : copycat_color(st.identity);
                    ImGui::TextColored(c.Value, "%s", row);
                }
            }
        }

        // 自动盖板(若辰同款判定)：监管走进板子判定区、自己站在板边、两人相距 ≤3 米时点一下交互键
        if (ImGui::CollapsingHeader(T("自动盖板###pallet", "Auto Pallet###pallet"))) {
            if (ImGui::BeginTable("##pallet_tbl", 2, ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("开启##auto_pallet", "Enable##auto_pallet"), &AutoPallet::enabled);   // 不能跟折叠框同名，同名 ID 冲突会点不动
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示判定范围", "Show Range"), &AutoPallet::show_range);
                ImGui::TableNextColumn();
                ImGui::Checkbox(T("显示触摸点", "Show Touch Point"), &AutoPallet::show_touch_point);
                ImGui::TableNextColumn();
                if (ImGui::Button(T("测试点击", "Test Tap"))) AutoPallet::request_test_tap();
                help_marker("站在板子旁边按，游戏里有反应说明触摸点对准了放板按钮",
                            "Stand next to a pallet and press it; if the game reacts, the touch point is on the pallet button");
                ImGui::EndTable();
            }

            // 触发宽度(游戏单位，判定矩形的全宽)：固定 / 随机(每次点完在上下限之间重抽)，范围都是 5~15
            ImGui::Checkbox(T("随机宽度##pallet_rand", "Random width##pallet_rand"), &AutoPallet::random_width);
            help_marker("每次点完在上下限之间重新抽一个触发宽度", "Re-roll the trigger width between Min and Max after every tap");
            if (!AutoPallet::random_width) {
                ImGui::SliderFloat(T("触发宽度##pallet_w", "Trigger width##pallet_w"), &AutoPallet::width,
                                   AutoPallet::WIDTH_MIN, AutoPallet::WIDTH_MAX, "%.1f");
            } else {
                ImGui::SliderFloat(T("下限##pallet_lo", "Min##pallet_lo"), &AutoPallet::width_lo,
                                   AutoPallet::WIDTH_MIN, AutoPallet::WIDTH_MAX, "%.1f");
                ImGui::SliderFloat(T("上限##pallet_hi", "Max##pallet_hi"), &AutoPallet::width_hi,
                                   AutoPallet::WIDTH_MIN, AutoPallet::WIDTH_MAX, "%.1f");
                if (AutoPallet::width_lo > AutoPallet::width_hi) AutoPallet::width_hi = AutoPallet::width_lo;   // 拖下限越过上限时上限跟着走
            }
            ImGui::SliderFloat(T("触摸点 X", "Touch X"), &AutoPallet::touch_x, 0.0f, (float)displayInfo.width, "%.0f");
            ImGui::SliderFloat(T("触摸点 Y", "Touch Y"), &AutoPallet::touch_y, 0.0f, (float)displayInfo.height, "%.0f");
            ImGui::SliderInt(T("按住(ms)", "Hold (ms)"), &AutoPallet::hold_ms, 10, 120);
            ImGui::SliderInt(T("位置随机(px)", "Jitter (px)"), &AutoPallet::jitter_px, 0, 60);   // 只作用于自动点击，测试点击不随机
            help_marker("自动点击落在触摸点周围这么多像素内的随机位置(测试点击不随机)",
                        "Auto taps land randomly within this many pixels of the touch point (test tap is exact)");
            ImGui::SliderInt(T("时长随机(±ms)", "Hold jitter (ms)"), &AutoPallet::jitter_ms, 0, 30);
            // 快速监管预判：实测速度 > 门槛时把监管位置往前推「提前量」再判(多数监管配表移速 4.54~4.71 m/s)
            ImGui::Checkbox(T("快速监管预判##pallet_pred", "Predict fast hunters##pallet_pred"), &AutoPallet::predict_fast);
            help_marker("监管实测速度超过门槛时，按「提前量」把它的位置往前推再判断(多数监管正常移速 4.5~4.7 m/s)",
                        "When a hunter moves faster than the threshold, project it forward by Lead before checking (normal speed is 4.5-4.7 m/s)");
            if (AutoPallet::predict_fast) {
                ImGui::SliderFloat(T("预判速度门槛(m/s)##pallet_pv", "Predict speed (m/s)##pallet_pv"), &AutoPallet::fast_mps, 4.0f, 10.0f, "%.1f");
                ImGui::SliderInt(T("提前量(ms)##pallet_lead", "Lead (ms)##pallet_lead"), &AutoPallet::lead_ms, 40, 800);
            }

            // 监管当前速度(按坐标实测，300ms 平滑)：联合狩猎每个监管一行
            {
                AutoPallet::HunterSpeed sp[PySelf::MAX_HUNTERS];
                int sn = AutoPallet::hunter_speeds(sp, PySelf::MAX_HUNTERS);
                if (sn == 0) ImGui::TextDisabled("%s", T("监管速度: —(开启自动或显示判定范围、我是求生者时测)",
                                                          "Hunter speed: - (measured when Enable or Show Range is on and you are a survivor)"));
                for (int k = 0; k < sn; k++) {
                    const char *name = "";
                    for (int i = 0; i < data_count; i++) if (data[i].obj == sp[k].obj) { name = data[i].str; break; }
                    if (!name[0]) name = T("监管", "Hunter");
                    if (sp[k].ok) ImTextT("监管速度: %s %.1f m/s", "Hunter speed: %s %.1f m/s", name, sp[k].mps);
                    else          ImTextT("监管速度: %s 测量中", "Hunter speed: %s measuring", name);
                }
            }
            ImTextT("状态: %s", "Status: %s", AutoPallet::status_text());
            ImTextT("点击 成功%d 失败%d  确认放下%d", "Tap ok%d fail%d confirmed%d",
                    AutoPallet::g_tap_ok.load(), AutoPallet::g_tap_fail.load(), AutoPallet::g_confirmed.load());
            ImTextT("上次: %s", "Last: %s", AutoPallet::last_result());
        }

        // 发布版本: 注入功能已停用
        // if (show_sohook) {
        //     ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        //     if (ImGui::CollapsingHeader("骨骼与进度")) {
        //         SoHook::RenderPanel(pid);
        //     }
        // }

        g_window = ImGui::GetCurrentWindow();
        ImGui::End();
    }

    // 监管天赋树(径向图)：独立可拖动/缩放窗口。数据用 Info.genius_levels[1..40]，纯显示。
    // 跟主菜单一样绑音量键(show_window)：音量下一起隐藏、音量上一起显示。
    if (show_genius_tree && show_window) {
        float side = displayInfo.height * 0.8f;          // 默认占 80% 屏高(正方形)
        ImGui::SetNextWindowSize(ImVec2(side, side), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(panel_alpha);
        if (cfg_apply_tree) {
            ImGui::SetNextWindowPos({cfg_tree_x, cfg_tree_y}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({cfg_tree_w, cfg_tree_h}, ImGuiCond_Always);
            cfg_apply_tree = false;
        }
        bool tree_open = ImGui::Begin(T("监管天赋树###gtree", "Hunter Talent Tree###gtree"), &show_genius_tree);
        cfg_tree_x = ImGui::GetWindowPos().x;  cfg_tree_y = ImGui::GetWindowPos().y;    // 保存配置时用
        cfg_tree_w = ImGui::GetWindowSize().x; cfg_tree_h = ImGui::GetWindowSize().y;
        if (tree_open) {
            // 联合狩猎两个监管：每个监管一页(标签名 = 监管名)，选中哪页画哪个(2026-10-25)
            uint64_t hbs[PySelf::MAX_HUNTERS];
            int hn = PyScene::in_battle() ? PySelf::butcher_bodies(hbs, PySelf::MAX_HUNTERS) : 0;
            static int tree_page = 0;
            if (hn >= 2 && ImGui::BeginTabBar("##gtree_tabs")) {
                for (int hi = 0; hi < hn; hi++) {
                    const char *name = "";
                    for (int i = 0; i < data_count; i++) if (data[i].obj == hbs[hi]) { name = data[i].str; break; }
                    char tab[64];
                    snprintf(tab, sizeof(tab), "%s###gtree_tab%d", name[0] ? name : T("监管", "Hunter"), hi);
                    if (ImGui::BeginTabItem(tab)) { tree_page = hi; ImGui::EndTabItem(); }
                }
                ImGui::EndTabBar();
            }
            if (tree_page >= hn) tree_page = 0;
            uint64_t hb = hn > 0 ? hbs[tree_page] : 0; PyGenius::Info gi;
            bool ok = hb != 0 && PyGenius::lookup((uintptr_t)hb, 0.f, 0.f, gi);
            if (ok) {
                int cnt = 0, term = 0, pts = 0;
                for (int id = 1; id <= 40; id++) {
                    int lv = gi.genius_levels[id];
                    if (!lv) continue; cnt++; pts += lv;
                    if (GeniusTree::is_terminal(id)) term++;
                }
                ImTextT("已点 %d 项   终端 %d/4   合计 %d 点", "Unlocked %d   terminal %d/4   total %d pts", cnt, term, pts);
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImVec2 avail = ImGui::GetContentRegionAvail();
                if (avail.x > 40 && avail.y > 40) {
                    ImVec2 center(p0.x + avail.x / 2.0f, p0.y + avail.y / 2.0f);
                    float scale = fminf(avail.x, avail.y) / 9.5f;   // 最大半径 depth3→4 单位 + 节点
                    DrawGeniusTree(ImGui::GetWindowDrawList(), center, scale, gi);
                    ImGui::Dummy(avail);
                }
            } else {
                ImGui::TextDisabled("%s", T("未在局内 / 监管天赋读取中…", "Not in match / reading hunter talents…"));
            }
        }
        ImGui::End();
    }
}
