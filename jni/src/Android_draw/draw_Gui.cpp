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
#include <linux/input.h>
#include <sstream>
#include <iomanip>
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

// ===== 固定信息：屏幕坐标 HUD 汇总，不依赖矩阵。每块独立开关 + 独立 X/Y/大小(size=文字像素高) =====
//   调试阶段用菜单里的滑动条摆位置/大小，调好后把下面的初值固化即可。
//   x<=0 的块(监管特质默认)表示"水平居中"，用 px 算。
static bool  fi_gen_on    = true;                                        // 密码机/大门 汇总面板(左中)
static float fi_gen_x     = 40.0f,   fi_gen_y    = 320.0f, fi_gen_size    = 25.0f;
static bool  fi_hunter_on = true;                                        // 监管 辅助特质 + 大天赋(顶中，只在我是求生者时显示)
static float fi_hunter_x  = 0.0f,    fi_hunter_y = 173.0f, fi_hunter_size = 30.0f;
static bool  fi_civ_on    = true;                                        // 求生者天赋(右上横排，只在我是监管时显示)
static float fi_civ_x     = 1894.0f, fi_civ_y    = 328.0f, fi_civ_size    = 28.0f, fi_civ_gap = 80.0f;

float z_x, z_y, z_z, d_x, d_y, d_z, camera, r_x, r_y, r_w;
float X1,Y1,X2,Y2,W,H,MIDDLE,TOP,BOTTOM;
float dist;   // 2026-09-23: int -> float。以前的截断迫使两处亚米级判断另外重算一遍
char objtext[256];
//char content[1024];
char Team[1024];
char Name[1024];
char prophet_text[1024];

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
            if (strstr(line, module_name) &&strstr(line, "r-xp")!= NULL &&(taddr-addr)== 114982912) {
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

int c;
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
                if (is_official && strstr(line, "."))    libbase = a;
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
    if (libbase < 0x5000000000){
        printf("[错误] libbase无效, 无法继续\n");
        sleep(9999);
    }
    c = (result.taddr-result.addr)/4096;
    long buff[512];
    while (MatrixOffset==0||ArrayaddrOffset==0)
    {
    	for (int i = 0; i < c; i++){
        	vm_readv(result.addr+(i*4096), &buff, 0x1000);
        	for (int ii=0;ii<512;ii+=1){

        	    if (MatrixOffset == 0 && *(long long*)(&buff[ii]) == 0x656A624F72655028LL){
        	        uint64_t sig_addr = result.addr + i*4096 + ii*8;
        	        // 2026-09-17 热更后 +0x430 恒为0。该处是一组步长0x88的指针，三个元素都能走到
        	        // 同一个矩阵对象，这次更新只是让它整体位移了0x10。写死任何一个下次还会废，
        	        // 改成在窗口内找"整条链都走得通"的槽位(p1有效 且 p1+0xa58 也有效)，自愈。
        	        // 注意上界：只判 >0x5000000000 会让浮点垃圾(如0x400674954293DF)也蒙混过关，
        	        // 本机用户态指针都在 0x77xx/0x79xx 段，统一卡 <0x8000000000。
        	        for (int off = 0x300; off <= 0x500; off += 8){
        	            uint64_t cand_slot = sig_addr + off;
        	            uint64_t p1 = getPtr64(cand_slot);            // getPtr64 已做 0xB4 掩码
        	            if (p1 <= 0x5000000000 || p1 >= 0x8000000000) continue;
        	            uint64_t p2 = getPtr64(p1 + 0xa58);
        	            if (p2 <= 0x5000000000 || p2 >= 0x8000000000) continue;
        	            MatrixOffset = cand_slot - libbase;
        	            printf("[矩阵命中] 签名=0x%llX 槽位=+0x%X 根=0x%llX MatrixOffset=0x%lx\n",
        	                   (unsigned long long)sig_addr, off, (unsigned long long)cand_slot, MatrixOffset);
        	            break;
        	        }
        	    }

        	    if (ArrayaddrOffset == 0 && buff[ii] == 16384){
                    if (getDword(result.addr + 4096*i + 8*ii - 0x8) == 257 &&
                        getFloat(result.addr + 4096*i + 8*ii - 16) == 1.0f){
                        ArrayaddrOffset = result.addr - libbase + i*4096 + ii*8 + 56;
                        printf("[数组命中] 偏移=0x%llX\n", (unsigned long long)ArrayaddrOffset);
                    }
                }
    	    }
        }
        if (MatrixOffset!=0 && ArrayaddrOffset!=0){
            uint64_t tmpArr = getPtr64(libbase + ArrayaddrOffset);
            uint64_t tmpEnd = getPtr64(libbase + ArrayaddrOffset + 8);
            if (tmpArr > 0x5000000000 && tmpEnd > tmpArr) break;
            printf("[数组无效] 重新扫描 Array=0x%llX End=0x%llX\n", (unsigned long long)tmpArr, (unsigned long long)tmpEnd);
            ArrayaddrOffset = 0;
        }
        sleep(5);
    }
    read_state = 2;

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
    // 自动盖板：独立线程 10ms 一轮，板子列表由下面的读取循环每轮发布过去
    AutoPallet::start();

    Arrayaddr = getPtr64(libbase + ArrayaddrOffset);
    uint64_t ArrayEnd = getPtr64(libbase + ArrayaddrOffset + 8);
    Count = (ArrayEnd - Arrayaddr) / 8;
    if (Count <= 0 || Count > 10000) Count = 1000;   // 兜底值：实测对局中 Count 只有 274~307
    printf("[数组] Arrayaddr=0x%llX End=0x%llX Count=%d\n", (unsigned long long)Arrayaddr, (unsigned long long)ArrayEnd, (int)Count);

    // 读空的轮次一律不发布：准备阶段 -> 进局那一下游戏会重建场景数组，偶尔一轮正好撞上：
    // 数组头读出来无效，或数组有效但一个对象都没收到。原来这两种情况都当场发布空列表，
    // 然后等 1~2 秒才扫下一轮 —— 表现就是"加载前明明都读到了，进局后全部消失两秒"。
    // 现在读空就保留上一轮(绘制侧照旧)，照常 2 秒后再扫。不需要"连续读空就清零"：
    // 每局都会重建数组，下一轮有效扫描会整体覆盖，不会有跨局残留。
    // 也不需要快速重试：对象地址一局内不变，上一轮可信就一直可信，重扫只是为了收新建的对象。
    while (true)
    {
    	uint64_t curArray = getPtr64(libbase + ArrayaddrOffset);
        uint64_t curArrayEnd = getPtr64(libbase + ArrayaddrOffset + 8);
        // 头、尾是两次独立读取，游戏恰好在中间扩容数组就会拿到新旧拼接的一对，
        // 算出来的数量是错的。再读一次头，对不上就当本轮无效
        if (getPtr64(libbase + ArrayaddrOffset) != curArray) curArray = 0;
        uint64_t curCount = curArrayEnd > curArray ? (curArrayEnd - curArray) / 8 : 0;
	    if (curArray < 0x5000000000 || curCount == 0 || curCount > 10000) {
            read_state = 1;   // 只影响面板状态文字，不影响绘制(Draw_Main 只在 0 时跳过)
            sleep(2);
            continue;
        }
        Arrayaddr = curArray;
        Count = curCount;
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
        for (int ii = 0; ii < Count && entity_count < 1000; ii++){
            cur_obj = getPtr64(curArray+0x8 * ii);	// 遍历数量次数            
                
    		if (cur_obj == 0)   			
        		continue;    			    			
    		
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
    	        if (len >= 256 || len == 0 || len < 0)
    	            continue;
    	        filter_class_name.resize(len);
    	        vm_readv(getPtr64(class_name_obj + 0x8), &filter_class_name[0], len);
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
    		    continue;
    		}
        	if (should_filter(filter_class_name)) {
        		continue;//过滤随从等无关对象
        	}
        	std::string s;
        	//预知监管者
            // 局内：CPython 侧有监管单位，直接取它 unit.model 对应的场景对象的类名。
            //   这样另一形态(无常黑白)、约瑟夫相机、大厅残留都不会顶掉真监管。
            // 准备阶段/大厅：units_by_type 里还没有监管单位(实测)，退回下面按类名匹配的老逻辑。
            //   ⚠ 准备阶段不能用 +0x73(visible) 过滤：打求生者时真监管的模型在准备界面是不可见的(实测)
            uint64_t real_hunter = 0;
            const bool has_real_hunter = PySelf::hunter_body(real_hunter);
            if (show_draw_prophet && has_real_hunter){
                if (cur_obj == real_hunter) snprintf(prophet_local, sizeof(prophet_local), "%s", getboss_prophet(filter_class_name.c_str()));
            }
            else if (show_draw_prophet){//预知开始
                if (strstr(filter_class_name.c_str(), "burke_console") == NULL&&strstr(filter_class_name.c_str(), "h55_joseph_camera") == NULL&&strstr(filter_class_name.c_str(), "redqueen_e_heijin_yizi") == NULL&&strstr(filter_class_name.c_str(), "_lod") == NULL){
                    if (strstr(filter_class_name.c_str(), "boss") != NULL){
                        s += getboss_prophet(filter_class_name.c_str());
                        snprintf(prophet_local, sizeof(prophet_local), "%s", s.c_str());
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
    			if (strstr(filter_class_name.c_str(), "boss") != NULL){
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
    			if (scene_result == NULL) continue;
    			strcpy(data[entity_count].str, scene_result);
    			data[entity_count].camp=3;
    			}
    			else if (strstr(filter_class_name.c_str(), "prop") != NULL){
    			const char* prop_result = getprop(filter_class_name.c_str());
    			if (prop_result == NULL) continue;
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
    			entity_count++;
			}
    			
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
        // 本轮一个对象都没收到：不发布，保留上一轮，按正常周期等下一轮(见循环前的说明)
        if (entity_count == 0) {
            sleep(2);
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
        if (prophet_local[0]) memcpy(prophet_text, prophet_local, sizeof(prophet_text));
        AutoPallet::publish_boards(boards_local, boards_n);
        data_count = entity_count;
        // 2026-09-23：3 秒 -> 2 秒。一轮实测约 9ms，读线程 CPU 0.3% -> 0.45%，
        // data[] 的撕裂窗口同比例从 0.3% 到 0.45%，都可忽略；换来新对象和预知监管更快出现
        sleep(2);
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
        // 名称前 2 字(全中文，每字 3 字节 → 取前 6 字节不切断)，亮底黑字
        char head[7] = {0};
        const char *nm = BUTCHER_GENIUS_NAME[id];
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
    int len = seat > 0 ? snprintf(out, cap, "%d号 ", seat) : 0;
    if (len < 0 || (size_t)len >= cap) len = 0;
    if (nm) snprintf(out + len, cap - len, "%s", nm);
    else if (identity) snprintf(out + len, cap - len, "身份%d", identity);
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
    return (identity >= 1 && identity <= 10) ? N[identity] : "?";
}

void Draw_Main(ImDrawList *Draw){
    PySelf::set_copycat_book(copycat_mode);        // 身份簿只在开关打开时记，见 PySelf.h
    if (libbase == 0 || read_state == 0) return;  // 数据未就绪，跳过本帧绘制
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
    bool matrix_fresh = false;
    {
        uint64_t root = getPtr64(libbase + MatrixOffset);
        uint64_t mobj = is_user_ptr(root) ? getPtr64(root + 0xa58) : 0;
        if (is_user_ptr(mobj)) {
            float tmp[16];
            if (vm_readv(mobj + 0x2c0, tmp, sizeof(tmp)) && matrix_finite(tmp)) {
                Matrix = mobj + 0x2c0;
                memcpy(matrix, tmp, sizeof(matrix));
                matrix_fresh = true;
            }
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
            ImGui::GetForegroundDrawList()->AddRect({bx1, by1}, {bx1 + bw, by1 + bh}, c, 3, 0, 1.8f);
            if (has_self) {                                             // 下：距离
                char dt[24];
                snprintf(dt, sizeof(dt), "%d 米", (int)d);
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
            const char *ok = "当前运行正常";
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
    if (show_draw_prophet && PySelf::in_lobby() && PyGenius::prep_as_hunter() && PyGenius::civ_size() > 0) {
        char line[320];
        int len = 0;
        int n = PyGenius::civ_size();
        for (int i = 0; i < n && len < (int)sizeof(line) - 48; i++) {
            int seat = 0, pid = 0; uint32_t bits = 0;
            if (!PyGenius::civ_at(i, seat, pid, bits) || bits == 0) continue;
            PyGenius::Info tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.camp = 2; tmp.genius_bits = bits; tmp.genius_state = PyGenius::GENIUS_COMPLETE;
            PyGenius::GeniusLine seg;
            PyGenius::genius_segments(tmp, seg);
            // GeniusLine 是 前/飞轮/后 三段(飞轮单独一段，局内要单独上色)，三段都要拼，漏了中间那段飞轮就没了
            int w = seat ? snprintf(line + len, sizeof(line) - len, "%s%d号[%s%s%s]", len ? "  " : "", seat, seg.pre, seg.flywheel, seg.post)
                         : snprintf(line + len, sizeof(line) - len, "%s?号[%s%s%s]", len ? "  " : "", seg.pre, seg.flywheel, seg.post);
            if (w < 0) break;
            len += w;
            if (len >= (int)sizeof(line)) { len = (int)sizeof(line) - 1; break; }   // 截断了，别让下一轮越界
        }
        if (len > 0) {
            auto ts2 = ImGui::CalcTextSize(line, 0, 25);
            Draw->AddText({px - (ts2.x / 2), 165}, ImColor(255, 200, 0, 255), line);
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
        Draw->AddText({tp.x + 46, tp.y - 12}, ImColor(0, 255, 255, 255), "盖板触摸点");
    }

    // ===== 固定信息(屏幕坐标 HUD 汇总，增量叠加，不改上面任何现有逻辑) =====
    // A. 密码机 / 大门 面板(左中)。破译未满 5 台时列有进度(>0)且未破译满的机器；
    //    全部破译完后改画两扇门进度(门数据是现有的 PyProgress 门缓冲)。
    if (fi_gen_on && PyProgress::available()){
        float x = fi_gen_x, y = fi_gen_y, sz = fi_gen_size;
        float bar_w = sz * 7.0f, bar_h = sz * 0.8f, gap = sz * 0.4f, line_h = sz + 6.0f;
        int decoded = PyProgress::decoded_count();
        if (decoded < 5){
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
                char lbl[24]; snprintf(lbl, sizeof(lbl), "门 %.0f%%", d.progress);
                fi_text(Draw, x + bar_w + gap, y + (bar_h - sz)/2.0f, sz, c, lbl);
                y += line_h;
            }
        }
    }

    // B. 监管 辅助特质 + 大天赋(默认顶中)。只在局内、监管单位存在时有内容。
    //    大天赋游戏不公开对手监管的，lookup 读到才画、读不到就空着。
    if (fi_hunter_on && PyScene::in_battle() && PySelf::camp() == 2){   // 我是求生者才看监管天赋
        uint64_t hb = 0;
        if (PySelf::hunter_body(hb)){
            PyGenius::Info gi;
            if (PyGenius::lookup((uintptr_t)hb, 0.f, 0.f, gi)){
                float sz = fi_hunter_size, line_h = sz + 6.0f, y = fi_hunter_y;
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
                }
            }
        }
    }

    // C. 求生者天赋(右上横排)。按绝处逢生三态着色(没带=白/带了没用=绿/已用=灰)。
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
    if (fi_civ_on && PyScene::in_battle() && PySelf::camp() == 1){   // 我是监管才看求生者天赋
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
                ImGui::GetForegroundDrawList()->AddRect({bx1, by1}, {bx1 + bw, by1 + bh}, c, 3, 0, 1.8f);
            if (show_draw_Distance) {
                char dt[24];
                snprintf(dt, sizeof(dt), "%d 米", (int)d);
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
                ImGui::GetForegroundDrawList()->AddRect({bx1, by1}, {bx1 + bw, by1 + bh}, c, 3, 0, 1.8f);
            if (show_draw_Distance) {
                char dt[24];
                snprintf(dt, sizeof(dt), "%d 米", (int)d);
                auto ds = ImGui::CalcTextSize(dt, 0, 25);
                Draw->AddText({bx1 + bw/2 - ds.x/2, by1 + bh + 10}, ImColor(255,200,0,255), dt);
            }
        }
    }

    // ---- 原生骨骼(抄若辰 sob_3A6074/39F0CC 偏移链,驱动读,零注入;见 NativeBones.h) ----
    if (show_draw_Bones && PySelf::available() && !PySelf::is_joseph_match()) {   // 约瑟夫局永远不画骨骼(用户要求,无视开关)
        uint64_t self_m = 0; PySelf::anchor(self_m);
        uint64_t hunter_m = 0; bool has_hunter = PySelf::hunter_body(hunter_m);
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
            // 监管红、求生者绿(监管本体 = PySelf::hunter_body)。约瑟夫镜世界里"另一个世界"的人画白色
            ImU32 bcol;
            if (PySelf::is_other_world(Ms[u]))      bcol = (ImU32)BotBoneColor;
            else if (has_hunter && Ms[u] == hunter_m) bcol = (ImU32)ImColor(255, 60, 60, 230);
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
            }
        }
    }

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
		// 只认 CPython 锚点给的阵营：兜底的相机深度启发式可能把求生者误判成监管，
		// 那样会让求生者视角下的监管整个消失，代价远大于多画一个。
		if (has_auth_self && self_camp == 1 && data[i].camp == 1) continue;
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
                test += " 米]  0x";
                test += objtext;    
                test += " [类名] ";
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
                    s += "[摄影机]";
                }
                auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
            }
            

		
    		if (data[i].camp==3)
    		{
    		    if (strstr(data[i].class_name, "dm65_scene_prop_30") != NULL){
    			    std::string s;
    		        if (show_draw_Door){
                        s += "[大门]";
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
                        s += "[道具箱]";
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
                        s += "[狂欢之椅]";
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, color_red, s.c_str());
    		    }
    		
    		    else if (strstr(data[i].class_name, "dm65_scene_prop_76") != NULL){
    			    std::string s;
    		        if (show_draw_Cellar){                                                  
                            s += "[地窖] ";
                            s += std::to_string((int) dist);    
                            s += " 米 ";
                    }
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, color_purple, s.c_str());
    		    }

    	    else if (strstr(data[i].class_name, "sender") != NULL){
    	        // 判据见上面 is_real_generator() 的注释。若发现密码机被破译完成后从叠加层消失，改那里。
    	        // 约瑟夫镜世界的镜像密码机(PyProgress 黑名单)不画，否则满屏重复的"[密码机]"空标记
    	        if (show_draw_secret_mechine && is_real_generator(data[i].obj) && !PyProgress::is_mirror_generator(data[i].obj)){
    	            // 进度来自 Python 侧的 GeneratorUnit，靠 model 指针身份配对(见 PyProgress.h)。
    	            // 配不上时(model 为空/链路失效)退化成原来的 "[密码机] X.X 米"。
    	            float progress = 0.f;
    	            bool has_progress = PyProgress::lookup(data[i].obj, D.X, D.Y, progress);
    	            if (!(has_progress && PyProgress::is_decoded(progress))){      // 破译完的机器本身和进度都不画
    	                std::ostringstream oss;
    	                oss << std::fixed << std::setprecision(1) << dist;
    	                std::string dist_text = " " + oss.str() + " 米";
    	                char head[24];
    	                if (has_progress) snprintf(head, sizeof(head), "[%.1f%%]", progress);
    	                else        snprintf(head, sizeof(head), "[密码机]");

    	                // 两段分开上色：头用进度色，距离沿用原来 61~63 米变绿的规则
    	                auto hs = ImGui::CalcTextSize(head, 0, 25);
    	                auto ds = ImGui::CalcTextSize(dist_text.c_str(), 0, 25);
    	                float x0 = r_x - (hs.x + ds.x) / 2.0f;
    	                Draw->AddText({x0, r_y}, has_progress ? progress_color(progress) : ImColor(255,255,255,255), head);
    	                Draw->AddText({x0 + hs.x, r_y},
    	                    ((int)dist >= 61 && (int)dist <= 63) ? color_green : ImColor(255, 255, 255, 255),
    	                    dist_text.c_str());

    	                // 进度条画在文字上方；进度为 0 时没有意义，不画
    	                if (has_progress && progress > 0.05f){
    	                    const float bar_w = 120.0f, bar_h = 10.0f;
    	                    float bx = r_x - bar_w / 2.0f, by = r_y - bar_h - 3.0f;
    	                    Draw->AddRectFilled({bx, by}, {bx + bar_w * (progress / 100.0f), by + bar_h}, progress_color(progress));
    	                    Draw->AddRect({bx, by}, {bx + bar_w, by + bar_h}, ImColor(255,255,255,255));
    	                }
    	            }
    	        }
    	    }
    		}
	
		    if (show_draw_Prop&&data[i].camp==4){
                // 自己身上的道具按距离剔除。必须只算**水平**距离:
                // 道具是挂在角色骨骼上的(手/胸口)，挂点比角色原点(脚底)高一截，
                // 实测 h55_pendant_glim(手电筒) 高度差固定 +0.73 米、水平只差 0.36 米。
                // 三维距离因此恒有 0.7+ 米的底噪，亚米阈值永远不成立 —— 挂件类道具靠
                // 三维距离**不可能**滤掉，跟站位无关，是结构性偏移。
                // 所以这里必须单独算水平距离，不能拿全局 dist(三维)比。
                float prop_hdist = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2)) / dist_scale;
                // 阈值 1.5 米：手电筒实测水平只差 0.36 米，但 1.0 米实战仍有漏网，
                // 说明别的挂件(火箭/橄榄球等)挂点更靠外。代价是贴身队友手里的道具
                // 也会被隐藏，1.5 米内的地面道具本来也在视野里，可以接受。
                if (prop_hdist >= 1.5f) {
                    const char* propName = getprop(data[i].class_name);
                    if (propName) {
                        std::string s = propName;
                        s += std::to_string((int) dist);
                        s += " 米";
                        auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                        Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), s.c_str());
                    }
                }
            }

            // 这里原本读一次 +0xaa(zy)，唯一用到它的是下面那段已停用的相机深度启发式，已删。
            if (!ShouldSkipEntity(data[i])){
                if (show_draw_Role&&strstr(data[i].class_name, "chr") != NULL){
                    std::string test;
                    test += " [";
                    test += std::to_string((int) dist);    
                    test += " 米]  0x";
                    test += objtext;    
                    test += " [类名] ";
                    test += data[i].class_name;
                    auto textSize = ImGui::CalcTextSize(test.c_str(), 0, 25);
                    Draw->AddText({r_x-(textSize.x/2),r_y}, ImColor(255,200,0,255), test.c_str());
                }
                			            
                // 旧的自身判定：相机深度落在 10~40 + zy + 阵营是1或2。**已停用**(见下)。
                // 原本只在 CPython 链路拿不到锚点时兜底，它的问题见 PySelf.h 文件头：
                //   - 任何一个求生者走进 10~40 这条深度带都会被误判成自身
                //   - zy 是 +0xaa，实测是通用状态位，**不区分是否自身**，挡不住
                //   - 自身锚错 -> Z 锚错 -> 全场距离全错，而且那个人会被 continue 掉不再绘制
                // 2026-09-22 停用：暂时只走 CPython 锚点(PySelf.h)，锚点拿不到时宁可没有自身也不猜。
                // if (!有权威自身 && camera < 40 && camera > 10 && zy&&(data[i].阵营==1||data[i].阵营==2)){
                //     自身 = data[i].obj;
                //     Z.X = D.X;
                //     Z.Z = D.Z;
                //     Z.Y = D.Y;
                //     自身阵营=对象阵营;
                //        continue;
                // }
               
                std::string s;

                // 离自己 1 米以内的角色不画方框和附带数据(名字/距离/天赋/特质/射线)：
                // 贴身的基本是自己身上的其它模型(备用形态、残留模型)，画出来只会挡视线。
                // 角色原点都在脚底，三维距离即可(道具那边是挂点偏高，才只能算水平距离)。
                // 没有自身锚点时 Z 不可信，不做这层过滤。
                bool too_close = (self_obj != 0) && dist < 1.0f;

                if (!too_close && (data[i].camp==1||data[i].camp==2)){
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
                        s += PySelf::body_in_photo_world(data[i].obj) ? " 镜像" : " 现实";
                    if (echo) name_color = BotBoneColor;   // 镜像投影：名字白
                    auto textSize = ImGui::CalcTextSize(s.c_str(), 0, 25);
                    Draw->AddText({X1 + W/2-(textSize.x/2),Y1-45}, name_color, s.c_str());
                    // 约瑟夫局：方框永远画(无视"绘制方框"开关)。镜像投影白 / 真实监管红 / 真实求生者绿。
                    // 非约瑟夫局(场上无 PhotoUnit，不会有投影)走原来的开关逻辑。
                    if (joseph){
                        ImColor bc = echo ? BotBoneColor
                                   : (cc ? name_color
                                   : (data[i].camp==1 ? BoneColor : color_green));
                        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, bc,3, 0,1.8f);
                    }
                    else if (show_draw_Rect){
        			    if (cc)
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, name_color,3, 0,1.8f);
        			    else if (IsGhostEntity(data[i]))
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, BotBoneColor,3, 0,1.8);
        			    else if (data[i].camp==1)
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, BoneColor,3, 0,1.8f);
        			    else if (data[i].camp==2)
        			        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, color_green,3, 0,1.8f);
        			}
                    float next_y = Y2 + 10;            // 距离、天赋、辅助特质依次往下排
                    const float line_h = ImGui::GetFontSize() + 2.0f;
                    if (show_draw_Distance || echo){   // 镜像投影默认画距离(白框+名字+距离固定集)
                        std::string dist_str;
                        dist_str += std::to_string((int) dist);
                        dist_str += " 米";
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
                                float w_pre = ImGui::CalcTextSize(seg.pre, 0, 25).x;
                                float w_flywheel = ImGui::CalcTextSize(seg.flywheel, 0, 25).x;
                                float w_post = ImGui::CalcTextSize(seg.post, 0, 25).x;
                                float tx = X1 + W/2 - (w_pre + w_flywheel + w_post)/2;
                                if (seg.pre[0])   Draw->AddText({tx, next_y}, c, seg.pre);
                                if (seg.flywheel[0]) Draw->AddText({tx + w_pre, next_y}, flywheel_color, seg.flywheel);
                                if (seg.post[0])   Draw->AddText({tx + w_pre + w_flywheel, next_y}, c, seg.post);
                                next_y += line_h;
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
	     
	                
	   //红夫人镜像                                   
	    if (mirror&&redqueenmod){
            if (getFloat(data[i].obj+0x1a0)==450&&data[i].camp==2){
                std::string ss;
                // 镜线在 Draw_Main 开头算好(放下的镜子 / 准备阶段预览镜子二选一)，这里只做一次关于直线的对称
                float orig_x = D.X, orig_y = D.Y;
                calculate_line_reflection(mirror_line_x1, mirror_line_y1, mirror_line_x2, mirror_line_y2, orig_x, orig_y, &D.X, &D.Y);
                camera = matrix[3] * D.X + matrix[7] * D.Z + matrix[11] * D.Y + matrix[15];
                dist = sqrt(pow(D.X - Z.X, 2) + pow(D.Y - Z.Y, 2) + pow(D.Z - Z.Z, 2)) / dist_scale;
                if (dist > 60) continue;   // 用户要求：过滤 60m 外的镜像求生者(镜像反射点到自身的距离)，别让远处镜像刷屏
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

                    ss += data[i].str;
                    auto textSize = ImGui::CalcTextSize(ss.c_str(), 0, 25);
                    Draw->AddText({X1 + W/2-(textSize.x/2),Y1-45}, BotBoneColor, ss.c_str());
                        
                    if (true){   // 方框默认画(不受"方框绘制"开关影响，用户要求)
                        ImGui::GetForegroundDrawList()->AddRect({X1, Y1},{X2, Y2}, BotBoneColor,3, 0,1.8f);
    			    }
    			        
                    if (show_draw_Distance){
                        std::string mirror_dist_str;
                        mirror_dist_str += std::to_string((int) dist);
                        mirror_dist_str += " 米";
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
                    // swap_yz 只在主骨骼轮读自身时校准；若"显示骨骼"关着主轮没跑，这里先拿自身补一次。
                    if (NativeBones::g_swap_yz.load() < 0) {
                        uint64_t self_cal = 0;
                        if (PySelf::anchor(self_cal)) { NativeBones::Skeleton tmp; NativeBones::read_unit(self_cal, true, tmp); }
                    }
                    NativeBones::Skeleton msk;
                    if (NativeBones::read_unit(data[i].obj, false, msk) && msk.valid) {
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

void Layout_tick_UI(bool *main_thread_flag) {
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

    if (show_window) {
        ImGui::Begin("New_Edition", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        if (::permeate_record_ini) {
            ImGui::SetWindowPos({LastCoordinate.Pos_x, LastCoordinate.Pos_y});
            ImGui::SetWindowSize({LastCoordinate.Size_x, LastCoordinate.Size_y});
            permeate_record_ini = false;
        }

        // 运行状态 + 结束进程放一个带边框的框里，不用展开折叠框就能退出
        ImGui::BeginChild("##status_box", ImVec2(0, 0),
                          ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);
        ImGui::Text("渲染模式 : %s, gui版本 : %s", graphics->RenderName, IMGUI_VERSION);
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 1.0f, 1.0f), "帧率 %.1f FPS", ImGui::GetIO().Framerate);

        // 读 smaps_rollup 要遍历页表，没必要每帧读，1 秒刷新一次
        static MemUsage mem;
        static double mem_time = -10.0;
        if (ImGui::GetTime() - mem_time >= 1.0) {
            mem = read_memory_usage();
            mem_time = ImGui::GetTime();
        }
        ImGui::Text("内存占用: %.1f MB", mem.total_kb() / 1024.0f);
        if (mem.gpu_ok)
            ImGui::Text("  进程 %.1f + 换出 %.1f + 显存 %.1f + 画面缓冲 %.1f",
                        mem.pss_kb / 1024.0f, mem.swap_kb / 1024.0f, mem.gpu_kb / 1024.0f, mem.buffer_kb / 1024.0f);
        else
            ImGui::Text("  进程 %.1f + 换出 %.1f + 画面缓冲 %.1f (显存未计)",
                        mem.pss_kb / 1024.0f, mem.swap_kb / 1024.0f, mem.buffer_kb / 1024.0f);

        ImGui::Text("数据状态:");
        if (read_state == 2)
            ImGui::TextColored(ImVec4(0.0f, 205.0f, 0.0f, 100.0f), "已获取到游戏数据");
        else if (read_state == 1)
            ImGui::TextColored(ImVec4(255.0f, 0.0f, 0.0f, 100.0f), "正在获取游戏数据");
        if (ImGui::Button("结束进程"))
            exit(0);
        ImGui::EndChild();

        if (ImGui::CollapsingHeader("基础信息")) {
            ImGui::Text("游戏进程:%d", pid);
            ImGui::Text("模块入口:%lx", libbase);
            ImGui::Text("游戏包名:%s", extractedString);
            ImGui::Text("矩阵地址:%lx", Matrix);
            ImGui::Text("数组地址:%lx", Arrayaddr);
            ImGui::Text("矩阵偏移:%lx", MatrixOffset);
            ImGui::Text("模块页数:%d", c);
            ImGui::Text("数组偏移:%lx", ArrayaddrOffset);
            ImGui::Text("监管者:%s", prophet_text);
            ImGui::Text("Py根:%s", PyRoot::status_text());
            ImGui::Text("密码机进度:%s (已破译%d)", PyProgress::status_text(), PyProgress::decoded_count());
            ImGui::Text("天赋:%s", PyGenius::status_text());
            ImGui::Text("阶段:%s", PyScene::status_text());
            // 自身锚点：链路一旦失效这里会写明原因，绘制自动退回相机深度启发式
            ImGui::Text("自身:%s", PySelf::status_text());
            ImGui::Text("镜世界:%s", PySelf::photo_diag_text());   // 约瑟夫 PhotoUnit 激活状态(_time/_players)
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("绘制设置")) {
            ImGui::Checkbox("显示鬼魂", &inform_ghost);
            ImGui::SameLine();
            // 预知监管和模仿者模式互斥(模仿者局没有监管)
            if (ImGui::Checkbox("预知监管", &show_draw_prophet) && show_draw_prophet) { copycat_mode = false; waterfight_mode = false; }

            ImGui::Checkbox("绘制道具", &show_draw_Prop);
            ImGui::SameLine();
            ImGui::Checkbox("夫人模式", &redqueenmod);

            ImGui::Checkbox("绘制调试", &Debugging);
            ImGui::SameLine();
            ImGui::Checkbox("显示密码机", &show_draw_secret_mechine);   // 局内进度/进度条 跟着它

            // 「显示天赋」「显示大门」已移到「固定信息」折叠框(用户要求)

            // 发布版本: 注入功能已停用
            // ImGui::Checkbox("骨骼与进度", &show_sohook);
            ImGui::Checkbox("显示骨骼", &show_draw_Bones);   // 原生骨骼,抄若辰链驱动读
            ImGui::SameLine();
            ImGui::Checkbox("显示方框", &show_draw_Rect);

            ImGui::Checkbox("监管天赋树", &show_genius_tree);   // 径向图，独立可拖动窗口(单独一行)
        }

        // 固定信息(屏幕 HUD 汇总)：每块独立开关 + 独立 X/Y/大小。调好位置后把初值固化即可
        if (ImGui::CollapsingHeader("固定信息")) {
            float mw = (float)displayInfo.width, mh = (float)displayInfo.height;

            // 局内绘制的天赋行 / 大门：各自独立 bool，跟下面的固定信息面板互不共用
            ImGui::Checkbox("显示天赋", &show_draw_Genius);   // 天赋行 + 监管辅助特质行(局内画框那套)
            ImGui::SameLine();
            ImGui::Checkbox("显示大门", &show_draw_Door);
            ImGui::Separator();

            ImGui::Checkbox("密码机/门面板##fi_gen", &fi_gen_on);
            if (fi_gen_on) {
                ImGui::SliderFloat("机 X##fi_gen", &fi_gen_x, 0.0f, mw, "%.0f");
                ImGui::SliderFloat("机 Y##fi_gen", &fi_gen_y, 0.0f, mh, "%.0f");
                ImGui::SliderFloat("机 大小##fi_gen", &fi_gen_size, 10.0f, 60.0f, "%.0f");
            }

            ImGui::Checkbox("监管特质/天赋##fi_hunter", &fi_hunter_on);
            if (fi_hunter_on) {
                ImGui::SliderFloat("监 X(0=居中)##fi_hunter", &fi_hunter_x, 0.0f, mw, "%.0f");
                ImGui::SliderFloat("监 Y##fi_hunter", &fi_hunter_y, 0.0f, mh, "%.0f");
                ImGui::SliderFloat("监 大小##fi_hunter", &fi_hunter_size, 10.0f, 60.0f, "%.0f");
            }

            ImGui::Checkbox("求生者天赋##fi_civ", &fi_civ_on);
            if (fi_civ_on) {
                ImGui::SliderFloat("求 X##fi_civ", &fi_civ_x, 0.0f, mw, "%.0f");
                ImGui::SliderFloat("求 Y##fi_civ", &fi_civ_y, 0.0f, mh, "%.0f");
                ImGui::SliderFloat("求 大小##fi_civ", &fi_civ_size, 10.0f, 60.0f, "%.0f");
                ImGui::SliderFloat("求 间距##fi_civ", &fi_civ_gap, 0.0f, 300.0f, "%.0f");
            }
        }


        // 模仿者模式：开关 + 身份簿。开着时名字换成"N号 身份"、不画射线；读到的身份记在簿子里，
        // 开会阶段页被换出读不到时沿用(见 PySelf.h 身份簿)。关掉开关簿子清空
        if (ImGui::CollapsingHeader("娱乐模式")) {
            // 林中童话(打水仗 BR)：独立绘制玩家(动物阵营配色)+物资。开着自动关模仿者和预知监管
            if (ImGui::Checkbox("林中童话", &waterfight_mode) && waterfight_mode) { copycat_mode = false; show_draw_prophet = false; }
            if (waterfight_mode) ImGui::Text("%s", PyWaterFight::status_text());
            if (ImGui::Checkbox("模仿者", &copycat_mode) && copycat_mode) { show_draw_prophet = false; waterfight_mode = false; }
            if (copycat_mode) {
                PySelf::CopycatBook book = PySelf::copycat_book();
                int known = 0, with_id = 0;
                for (int k = 0; k < PySelf::MAX_COPYCAT; k++)
                    if (book.seat[k].known) { known++; if (book.seat[k].identity) with_id++; }
                ImGui::Text("已记录 %d 人，身份已知 %d 人", known, with_id);
                for (int k = 0; k < PySelf::MAX_COPYCAT; k++) {
                    const PySelf::CopycatSeat &st = book.seat[k];
                    if (!st.known) continue;
                    const char *nm = PySelf::copycat_identity_name(st.identity);
                    char row[64];
                    int len = snprintf(row, sizeof(row), "%2d号 ", k + 1);
                    if (nm) len += snprintf(row + len, sizeof(row) - len, "%s", nm);
                    else if (st.identity) len += snprintf(row + len, sizeof(row) - len, "身份%d", st.identity);
                    else len += snprintf(row + len, sizeof(row) - len, "?");
                    if (book.self_seat == k + 1) len += snprintf(row + len, sizeof(row) - len, "  (我)");
                    if (st.dead) snprintf(row + len, sizeof(row) - len, "  已死");
                    ImColor c = st.dead ? ImColor(140,140,140,255) : copycat_color(st.identity);
                    ImGui::TextColored(c.Value, "%s", row);
                }
            }
        }

        // 自动盖板(若辰同款判定)：监管走进板子判定区、自己站在板边、两人相距 ≤3 米时点一下交互键
        if (ImGui::CollapsingHeader("自动盖板")) {
            ImGui::Checkbox("开启##auto_pallet", &AutoPallet::enabled);   // 不能跟折叠框同名，同名 ID 冲突会点不动
            ImGui::SameLine();
            ImGui::Checkbox("显示判定范围", &AutoPallet::show_range);
            ImGui::Checkbox("显示触摸点", &AutoPallet::show_touch_point);
            ImGui::SameLine();
            if (ImGui::Button("测试点击")) AutoPallet::request_test_tap();

            static const char *modes[] = {"暴力(宽12)", "演戏(宽8)", "随机(8~12)"};
            ImGui::Combo("模式", &AutoPallet::mode, modes, 3);
            ImGui::SliderFloat("触摸点 X", &AutoPallet::touch_x, 0.0f, (float)displayInfo.width, "%.0f");
            ImGui::SliderFloat("触摸点 Y", &AutoPallet::touch_y, 0.0f, (float)displayInfo.height, "%.0f");
            ImGui::SliderInt("按住(ms)", &AutoPallet::hold_ms, 10, 120);
            ImGui::SliderInt("位置随机(px)", &AutoPallet::jitter_px, 0, 60);   // 只作用于自动点击，测试点击不随机
            ImGui::SliderInt("时长随机(±ms)", &AutoPallet::jitter_ms, 0, 30);

            ImGui::Text("状态: %s", AutoPallet::status_text());
            ImGui::Text("点击 成功%d 失败%d  确认放下%d", AutoPallet::g_tap_ok.load(), AutoPallet::g_tap_fail.load(),
                        AutoPallet::g_confirmed.load());
            ImGui::Text("上次: %s", AutoPallet::last_result());
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
        if (ImGui::Begin("监管天赋树", &show_genius_tree)) {
            uint64_t hb = 0; PyGenius::Info gi;
            bool ok = PyScene::in_battle() && PySelf::hunter_body(hb)
                      && PyGenius::lookup((uintptr_t)hb, 0.f, 0.f, gi);
            if (ok) {
                int cnt = 0, term = 0, pts = 0;
                for (int id = 1; id <= 40; id++) {
                    int lv = gi.genius_levels[id];
                    if (!lv) continue; cnt++; pts += lv;
                    if (GeniusTree::is_terminal(id)) term++;
                }
                ImGui::Text("已点 %d 项   终端 %d/4   合计 %d 点", cnt, term, pts);
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImVec2 avail = ImGui::GetContentRegionAvail();
                if (avail.x > 40 && avail.y > 40) {
                    ImVec2 center(p0.x + avail.x / 2.0f, p0.y + avail.y / 2.0f);
                    float scale = fminf(avail.x, avail.y) / 9.5f;   // 最大半径 depth3→4 单位 + 节点
                    DrawGeniusTree(ImGui::GetWindowDrawList(), center, scale, gi);
                    ImGui::Dummy(avail);
                }
            } else {
                ImGui::TextDisabled("未在局内 / 监管天赋读取中…");
            }
        }
        ImGui::End();
    }
}
