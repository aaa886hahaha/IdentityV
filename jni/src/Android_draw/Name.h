#pragma once
#include <unordered_map>
#include <string>
#include <cstring>

// ==================== 缓存 ====================
static std::unordered_map<std::string, const char*> player_cache;
static std::unordered_map<std::string, const char*> boss_cache;

// ==================== 映射表结构 ====================
struct NameMapping {
    const char* keyword;
    const char* display;
};

// ==================== 求生者映射表 ====================
static const NameMapping player_table[] = {
    {"detective", "侦探"},
    {"deluosi_dress_ghost", "联觉记者"},
    {"w_xiangshuishi", "调香师"},
    {"w_bdz", "空军"},
    {"m_yxz_bo", "佣兵"},
    {"_m_bo", "幸运儿"},
    {"m_qiutu", "囚徒"},
    {"yiyaoshi", "医生"},
    {"m_xcz", "冒险家"},
    {"w_cp", "心理学家"},
    {"m_cp", "病患"},
    {"w_hlz", "园丁"},
    {"m_zbs", "先知"},
    {"w_fxt", "机械师"},
    {"tqz", "机械师"},
    {"h55_survivor_puppet", "机器人"},
    {"m_shoumu", "守墓人"},
    {"m_ydy", "前锋"},
    {"w_wht", "舞女"},
    {"m_niuzai", "牛仔"},
    {"m_rls", "入殓师"},
    {"w_jyz", "盲女"},
    {"m_yc", "邮差"},
    {"w_jisi", "祭司"},
    {"m_ldz", "魔术师"},
    {"girl", "小女孩"},
    {"w_zhoushu", "咒术师"},
    {"m_kantan", "勘探员"},
    {"m_yeren", "野人"},
    {"m_zaji", "杂技演员"},
    {"m_dafu", "大副"},
    {"w_tiaojiu", "调酒师"},
    {"w_kunchong", "昆虫学家"},
    {"m_artist", "画家"},
    {"m_jiqiu", "击球手"},
    {"w_shangren", "玩具商"},
    {"m_bzt", "小说家"},
    {"m_yinyue", "作曲家"},
    {"m_spjk", "哭泣小丑"},
    {"huojian", "哭泣小丑"},
    {"w_gd", "古董商"},
    {"m_niexi", "教授"},
    {"m_fxj", "飞行家"},
    {"w_deluosi", "记者"},
    {"m_muou", "木偶师"},
    {"m_it", "律师"},
    {"m_qd", "慈善家"},
    {"w_ll", "拉拉队员"},
    {"w_fl", "法罗女士"},
    {"m_xf", "火灾调查员"},
    {"m_dxzh", "骑士"},
    {"w_qx", "气象学家"},
    {"rice", "囚徒"},
    {"w_gjs", "弓箭手"},
    {"w_hds", "幻灯师"},
    {"m_ttds", "逃脱大师"},
    {"dns", "斗牛士"},
    {"w_moju", "默剧艺人"},
    {"boy", "神童"}
    
};

// ==================== 监管者映射表 ====================
static const NameMapping boss_table[] = {
    {"joseph", "[约瑟夫]"},
    {"banshee", "[红蝶]"},
    {"butcher_sxwd", "[小丑]"},
    {"chuanhuo", "[厂长残火]"},
    {"redqueen", "[红夫人]"},
    {"dm65_butcher_ll", "[鹿头]"},
    {"ripper", "[杰克]"},
    {"spider", "[蜘蛛]"},
    {"lizard", "[孽蜥]"},
    {"hastur", "[黄衣之主]"},
    {"burke", "[疯眼]"},
    {"yidhra_xintu", "[信徒]"},
    {"yidhra", "[梦之女巫]"},
    {"trap", "[夹子]"},
    {"boy", "[爱哭鬼]"},
    {"bomber", "[26号守卫]"},
    {"bonbon", "[26号守卫]"},
    {"messager", "[噩梦]"},
    {"joan", "[使徒]"},
    {"paganini", "[小提琴家]"},
    {"sculptor", "[雕刻家]"},
    {"polun", "[破轮]"},
    {"frank", "[博士]"},
    {"yunv", "[渔女]"},
    {"laxiang", "[蜡像师]"},
    {"wax", "[蜡像师]"},
    {"lady", "[记录员]"},
    {"ithaqua", "[守夜人]"},
    {"famingjia", "[隐士]"},
    {"hermit", "[隐士]"},
    {"goat", "[歌剧演员]"},
    {"spkantan", "[愚人金]"},
    {"space", "[跛脚羊]"},
    {"spzaji", "[喧嚣]"},
    {"grocer", "[杂货商]"},
    {"billy", "[台球手]"},
    {"spkunc", "[女王蜂]"},
    {"beast", "[心兽]"},
    {"dentist", "[牙医]"},
};

// ==================== 道具映射表 ====================
static const NameMapping prop_table[] = {
    {"inject", "镇静剂"},
    {"moshubang", "魔术棒"},
    {"flaregun", "信号枪"},
    {"jiutong", "多夫林"},
    {"huzhou", "护肘"},
    {"map", "地图"},
    {"book", "小人书"},
    {"gjx", "工具箱"},
    {"glim", "手电筒"},
    {"xiangshuiping", "忘忧之香"},
    {"controller", "遥控器"},
    {"football", "橄榄球"},
    {"huaibiao", "怀表"},
    {"puppet", "厂长傀儡"},
    {"tower", "窥视者"},
    {"huojian", "哭丑火箭"},
    {"patro", "巡视者"},
};

// ==================== 通用查找函数 ====================
template<size_t N>
static inline const char* table_lookup(const char* class_name, const NameMapping (&table)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (strstr(class_name, table[i].keyword)) {
            return table[i].display;
        }
    }
    return nullptr;
}

// ==================== 求生者查找 (带缓存) ====================
static const char* getplayer(const char* class_name) {
    if (!class_name || !class_name[0]) return "[未知]";
    
    // 查缓存
    auto it = player_cache.find(class_name);
    if (it != player_cache.end()) {
        return it->second;
    }
    
    // 未命中，遍历表查找
    const char* result = table_lookup(class_name, player_table);
    // 未知角色：返回调用方传进来的原始类名，但**绝不能缓存**。
    // 调用方给的是全局 std::string filter_class_name 的内部缓冲(draw_Gui.cpp:656 每个对象
    // resize+读入)，缓存下来等于存了一个别人家的指针：容量一扩(启动头几帧会扩几次)旧缓冲就被释放，
    // 之后命中这条缓存就是 use-after-free —— strcpy 到 data[].str 时轻则乱码，
    // 重则读不到 '\0' 越界写坏相邻的 data[] 项。缓存里只允许放静态表里的常量指针。
    if (!result) return class_name;

    // 存入缓存(值是 player_table 里的静态字符串，生命周期同进程)
    player_cache[class_name] = result;

    return result;
}

// ==================== 监管者查找 (带缓存) ====================
static const char* getboss(const char* class_name) {
    if (!class_name || !class_name[0]) return "[未知]";
    
    // 查缓存
    auto it = boss_cache.find(class_name);
    if (it != boss_cache.end()) {
        return it->second;
    }
    
    // 未命中，先处理特殊情况
    const char* result = nullptr;
    
    // 无常：区分黑白
    if (strstr(class_name, "wuchang")) {
        result = strstr(class_name, "white") ? "[白无常]" : "[黑无常]";
    }
    // 伊斯：区分本体和信徒
    else if (strstr(class_name, "yith")) {
        result = strstr(class_name, "ghost") ? "[伊斯人]" : "[时空之影]";
    }
    // 厂长：特殊路径匹配（排除小丑 butcher_sxwd、鹿头 dm65_butcher_ll，这两个资源都挂在厂长目录下）
    else if ((strstr(class_name, "butcher.gim") || strstr(class_name, "boss\\butcher"))
             && !strstr(class_name, "_lod.gim")
             && !strstr(class_name, "butcher_sxwd")  // 排除小丑
             && !strstr(class_name, "dm65_butcher_ll")) {  // 排除鹿头
        result = "[厂长]";
    }
    // 常规查表
    else {
        result = table_lookup(class_name, boss_table);
    }
    
    // 同 getplayer：未知监管返回原始类名，但不缓存(那是调用方的临时缓冲，会被释放)
    if (!result) return class_name;

    // 存入缓存(值是静态字符串常量)
    boss_cache[class_name] = result;

    return result;
}

// ==================== 预知监管用的名字 ====================
// 预知只关心"对面是哪个监管"，从属单位要归回本体：信徒 -> 梦之女巫、伊斯人(幻影) -> 时空之影。
// 场上绘制仍用 getboss() 的原名，信徒/幻影要和本体分开显示
static const char* getboss_prophet(const char* class_name) {
    const char* name = getboss(class_name);
    if (strcmp(name, "[信徒]") == 0) return "[梦之女巫]";
    if (strcmp(name, "[伊斯人]") == 0) return "[时空之影]";
    return name;
}

// ==================== 道具查找 (不缓存，出现少) ====================
static const char* getprop(const char* class_name) {
    if (!class_name || !class_name[0]) return nullptr;
    return table_lookup(class_name, prop_table);
}

// ==================== 场景查找 ====================
static const char* getscene(const char* class_name) {
    if (!class_name) return nullptr;
    if (strstr(class_name, "prop_76")) return "地窖";
    if (strstr(class_name, "sender")) return "电机";
    return nullptr;
}
