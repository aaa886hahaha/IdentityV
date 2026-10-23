#pragma once
#include <unordered_map>
#include <string>
#include <cstring>

// 语言开关：false=中文(默认)，true=英文。由悬浮窗「结束进程」旁的按钮切换(draw_Gui.cpp)。
// 英文名一律用官方术语，不硬翻：角色取官网职业名(identityvgame.com 角色页)，道具/从属单位取 Fandom Wiki。
inline bool g_english = false;

// 界面文案本地化助手：g_english 时返回 en，否则返回 cn。
// 用法：ImGui::Checkbox(T("方框","Box"), &x)；带格式串同理：ImGui::Text(T("进程:%d","PID:%d"), pid)。
// 返回的都是字符串常量指针，生命周期同进程，可直接喂给 ImGui。
static inline const char* T(const char* cn, const char* en) { return g_english ? en : cn; }

// ==================== 映射表结构(中英双栏) ====================
struct NameMapping {
    const char* keyword;
    const char* cn;
    const char* en;
};
static inline const char* pick(const NameMapping* m) { return g_english ? m->en : m->cn; }

// ==================== 缓存(存映射项指针,切换语言即时生效) ====================
static std::unordered_map<std::string, const NameMapping*> player_cache;
static std::unordered_map<std::string, const NameMapping*> boss_cache;

// ==================== 求生者映射表 ====================
static const NameMapping player_table[] = {
    {"detective",             "侦探",     "Detective"},
    {"deluosi_dress_ghost",   "联觉记者", "Journalist"},     // 记者的联觉形态
    {"w_xiangshuishi",        "调香师",   "Perfumer"},
    {"w_bdz",                 "空军",     "Coordinator"},
    {"m_yxz_bo",              "佣兵",     "Mercenary"},
    {"_m_bo",                 "幸运儿",   "Lucky Guy"},
    {"m_qiutu",               "囚徒",     "Prisoner"},
    {"yiyaoshi",              "医生",     "Doctor"},
    {"m_xcz",                 "冒险家",   "Explorer"},
    {"w_cp",                  "心理学家", "Psychologist"},
    {"m_cp",                  "病患",     "Patient"},
    {"w_hlz",                 "园丁",     "Gardener"},
    {"m_zbs",                 "先知",     "Seer"},
    {"w_fxt",                 "机械师",   "Mechanic"},
    {"tqz",                   "机械师",   "Mechanic"},
    {"h55_survivor_puppet",   "机器人",   "Mechanic's Doll"}, // 机械师傀儡，国际服 TID_DES_CHARACTER_JIXIESHI_1_4
    {"m_shoumu",              "守墓人",   "Grave Keeper"},
    {"m_ydy",                 "前锋",     "Forward"},
    {"w_wht",                 "舞女",     "Female Dancer"},
    {"m_niuzai",              "牛仔",     "Cowboy"},
    {"m_rls",                 "入殓师",   "Embalmer"},
    {"w_jyz",                 "盲女",     "The Mind's Eye"},
    {"m_yc",                  "邮差",     "Postman"},
    {"w_jisi",                "祭司",     "Priestess"},
    {"m_ldz",                 "魔术师",   "Magician"},
    {"girl",                  "小女孩",   "Little Girl"},
    {"w_zhoushu",             "咒术师",   "Enchantress"},
    {"m_kantan",              "勘探员",   "Prospector"},
    {"m_yeren",               "野人",     "Wildling"},
    {"m_zaji",                "杂技演员", "Acrobat"},
    {"m_dafu",                "大副",     "First Officer"},
    {"w_tiaojiu",             "调酒师",   "Barmaid"},
    {"w_kunchong",            "昆虫学家", "Entomologist"},
    {"m_artist",              "画家",     "Painter"},
    {"m_jiqiu",               "击球手",   "Batter"},
    {"w_shangren",            "玩具商",   "Toy Merchant"},
    {"m_bzt",                 "小说家",   "Novelist"},
    {"m_yinyue",              "作曲家",   "Composer"},
    {"m_spjk",                "哭泣小丑", "Weeping Clown"},
    {"huojian",               "哭泣小丑", "Weeping Clown"},
    {"w_gd",                  "古董商",   "Antiquarian"},
    {"m_niexi",               "教授",     "Professor"},
    {"m_fxj",                 "飞行家",   "Aeroplanist"},
    {"w_deluosi",             "记者",     "Journalist"},
    {"m_muou",                "木偶师",   "Puppeteer"},
    {"m_it",                  "律师",     "Lawyer"},
    {"m_qd",                  "慈善家",   "Thief"},
    {"w_ll",                  "拉拉队员", "Cheerleader"},
    {"w_fl",                  "法罗女士", "Faro Lady"},
    {"m_xf",                  "火灾调查员","Fire Investigator"},
    {"m_dxzh",                "骑士",     "Knight"},
    {"w_qx",                  "气象学家", "Meteorologist"},
    {"rice",                  "囚徒",     "Prisoner"},
    {"w_gjs",                 "弓箭手",   "Archer"},
    {"w_hds",                 "幻灯师",   "Lanternist"},
    {"m_ttds",                "逃脱大师", "Escapologist"},
    {"dns",                   "斗牛士",   "Matador"},
    {"w_moju",                "默剧艺人", "Mime Artist"},
    {"boy",                   "神童",     "Prodigy"}
};

// ==================== 监管者映射表 ====================
// 英文用官方「职业别名」(与红蝶=Geisha 同风格),不用人名。
static const NameMapping boss_table[] = {
    {"joseph",          "[约瑟夫]",   "[Photographer]"},
    {"banshee",         "[红蝶]",     "[Geisha]"},
    {"butcher_sxwd",    "[小丑]",     "[Smiley Face]"},
    {"chuanhuo",        "[厂长残火]", "[Hell Ember]"},
    {"redqueen",        "[红夫人]",   "[Bloody Queen]"},
    {"dm65_butcher_ll", "[鹿头]",     "[Gamekeeper]"},
    {"ripper",          "[杰克]",     "[The Ripper]"},
    {"spider",          "[蜘蛛]",     "[Soul Weaver]"},
    {"lizard",          "[孽蜥]",     "[Evil Reptilian]"},
    {"hastur",          "[黄衣之主]", "[The Feaster]"},
    {"burke",           "[疯眼]",     "[Mad Eyes]"},
    {"yidhra_xintu",    "[信徒]",     "[Follower]"},
    {"yidhra",          "[梦之女巫]", "[Dream Witch]"},
    {"trap",            "[夹子]",     "[Trap]"},              // 国际服 TID_NAME_TRAP_LL
    {"boy",             "[爱哭鬼]",   "[Axe Boy]"},
    {"bomber",          "[26号守卫]", "[Guard 26]"},
    {"bonbon",          "[26号守卫]", "[Guard 26]"},
    {"messager",        "[噩梦]",     "[Nightmare]"},
    {"joan",            "[使徒]",     "[Disciple]"},
    {"paganini",        "[小提琴家]", "[Violinist]"},
    {"sculptor",        "[雕刻家]",   "[Sculptor]"},
    {"polun",           "[破轮]",     "[Breaking Wheel]"},
    {"frank",           "[博士]",     "[Undead]"},
    {"yunv",            "[渔女]",     "[Naiad]"},
    {"laxiang",         "[蜡像师]",   "[Wax Artist]"},
    {"wax",             "[蜡像师]",   "[Wax Artist]"},
    {"lady",            "[记录员]",   "[Clerk]"},
    {"ithaqua",         "[守夜人]",   "[Night Watch]"},
    {"famingjia",       "[隐士]",     "[Hermit]"},
    {"hermit",          "[隐士]",     "[Hermit]"},
    {"goat",            "[歌剧演员]", "[Opera Singer]"},
    {"spkantan",        "[愚人金]",   "[Fool's Gold]"},
    {"space",           "[跛脚羊]",   "[Goatman]"},
    {"spzaji",          "[喧嚣]",     "[Hullabaloo]"},
    {"grocer",          "[杂货商]",   "[Peddler]"},
    {"billy",           "[台球手]",   "[Cueist]"},
    {"spkunc",          "[女王蜂]",   "[Queen Bee]"},
    {"beast",           "[心兽]",     "[Herztier]"},
    {"dentist",         "[牙医]",     "[Dentist]"},
};

// ==================== 道具映射表 ====================
// 道具英文名取 Fandom Wiki(Items 页 + 各角色 object 字段 + Traits 页)。
static const NameMapping prop_table[] = {
    {"inject",        "镇静剂",   "Syringe"},
    {"moshubang",     "魔术棒",   "Decoy"},
    {"flaregun",      "信号枪",   "Flare Gun"},
    {"jiutong",       "多夫林",   "Dovlin"},
    {"huzhou",        "护肘",     "Elbow Pads"},
    {"map",           "地图",     "Map"},
    {"book",          "小人书",   "Travel Journals"},
    {"gjx",           "工具箱",   "Toolkit"},
    {"glim",          "手电筒",   "Flashlight"},
    {"xiangshuiping", "忘忧之香", "Euphoria"},
    {"controller",    "遥控器",   "Controller"},
    {"football",      "橄榄球",   "Rugby Ball"},
    {"huaibiao",      "怀表",     "Pocket Watch"},    // 国际服 TID_WATCH_LV1
    {"puppet",        "厂长傀儡", "Puppet"},
    {"tower",         "窥视者",   "Peeper"},
    {"huojian",       "哭丑火箭", "Rocket"},
    {"patro",         "巡视者",   "Patroller"},
};

// ==================== 通用查找函数 ====================
template<size_t N>
static inline const NameMapping* table_lookup(const char* class_name, const NameMapping (&table)[N]) {
    for (size_t i = 0; i < N; ++i) {
        if (strstr(class_name, table[i].keyword)) {
            return &table[i];
        }
    }
    return nullptr;
}

// ==================== 求生者查找 (带缓存) ====================
static const char* getplayer(const char* class_name) {
    if (!class_name || !class_name[0]) return g_english ? "[Unknown]" : "[未知]";

    auto it = player_cache.find(class_name);
    if (it != player_cache.end()) {
        return pick(it->second);
    }

    const NameMapping* m = table_lookup(class_name, player_table);
    // 未知角色：返回调用方传进来的原始类名，但**绝不能缓存**(那是调用方临时 std::string 缓冲,
    // 扩容后释放 → 缓存里成悬空指针 → use-after-free)。缓存里只放静态表项指针。
    if (!m) return class_name;

    player_cache[class_name] = m;
    return pick(m);
}

// ==================== 监管者查找 (带缓存) ====================
static const char* getboss(const char* class_name) {
    if (!class_name || !class_name[0]) return g_english ? "[Unknown]" : "[未知]";

    auto it = boss_cache.find(class_name);
    if (it != boss_cache.end()) {
        return pick(it->second);
    }

    // 特殊情况先处理(这些不进表、不缓存，直接按开关返回)
    // 无常：区分黑白(White Guard 谢必安 / Black Guard 范无救)
    if (strstr(class_name, "wuchang")) {
        if (strstr(class_name, "white")) return g_english ? "[White Guard]" : "[白无常]";
        return g_english ? "[Black Guard]" : "[黑无常]";
    }
    // 伊斯：本体和幻影
    if (strstr(class_name, "yith")) {
        if (strstr(class_name, "ghost")) return g_english ? "[Yithian]" : "[伊斯人]";
        return g_english ? "[The Shadow]" : "[时空之影]";
    }
    // 厂长：特殊路径(排除小丑 butcher_sxwd、鹿头 dm65_butcher_ll，两者资源都挂厂长目录)
    if ((strstr(class_name, "butcher.gim") || strstr(class_name, "boss\\butcher"))
        && !strstr(class_name, "_lod.gim")
        && !strstr(class_name, "butcher_sxwd")
        && !strstr(class_name, "dm65_butcher_ll")) {
        return g_english ? "[Hell Ember]" : "[厂长]";
    }

    // 常规查表
    const NameMapping* m = table_lookup(class_name, boss_table);
    if (!m) return class_name;   // 未知监管返回原始类名,不缓存

    boss_cache[class_name] = m;
    return pick(m);
}

// ==================== 预知监管用的名字 ====================
// 预知只关心"对面是哪个监管"，从属单位归回本体：信徒->梦之女巫、伊斯人(幻影)->时空之影。
static const char* getboss_prophet(const char* class_name) {
    const char* name = getboss(class_name);
    if (strcmp(name, "[信徒]") == 0)   return "[梦之女巫]";
    if (strcmp(name, "[Follower]") == 0) return "[Dream Witch]";
    if (strcmp(name, "[伊斯人]") == 0) return "[时空之影]";
    if (strcmp(name, "[Yithian]") == 0) return "[The Shadow]";
    return name;
}

// ==================== 道具查找 (不缓存，出现少) ====================
static const char* getprop(const char* class_name) {
    if (!class_name || !class_name[0]) return nullptr;
    const NameMapping* m = table_lookup(class_name, prop_table);
    return m ? pick(m) : nullptr;
}

// ==================== 场景查找 ====================
static const char* getscene(const char* class_name) {
    if (!class_name) return nullptr;
    if (strstr(class_name, "prop_76")) return g_english ? "Dungeon" : "地窖";
    if (strstr(class_name, "sender"))  return g_english ? "Cipher Machine" : "电机";
    return nullptr;
}
