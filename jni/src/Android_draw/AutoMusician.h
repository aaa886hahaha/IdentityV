#ifndef IDV_AUTO_MUSICIAN_H
#define IDV_AUTO_MUSICIAN_H

// ============================================================================
// 作曲家(MUSICIAN, pid 114)修机节奏自动 —— 只做修机两条轨道(左=CHANNEL_1 / 右=CHANNEL_2)，不管技能轨道 3
// 2026-11-15 静态，skill idv-reverse-resume/auto-qte.md「作曲家」两节
// ============================================================================
//
// 游戏判定(common_cs/items/ItemMusicStick.pyc QTEHandler.qte，客户端判、服务端不复核)：
//   qte_p = note.begin_time + music_qte_length − offset_adjust / move_speed
//   d = now − qte_p：d < −valid_torrence 不算；d < −torrence 失败；|d| < torrence 成功；否则漏
//   修机 qte_mode == ABILITY_QTE(1)：music_qte_length = music_qte_length_ability[difficulty]，torrence = client_torrence_ability
//   offset_adjust = 游戏设置里的「判定线偏移」，move_speed 恒 1.0
// 当前游戏时间：静态是 item.room(=battle_world).pause_mgr._abs_game_time(time.time() + 常数)，
//   但上机实测音符钟比它稳定快 500ms(PauseMgr.game_time 是 cython_opt_method，运行时可能不是反编译那份)
//   → 用音符自己校：入队那帧 now ≈ begin_time，now = CLOCK_REALTIME + C，C 取样本中位数(±5ms)
// 音符：binded_musician_item.music_pool.qte_container_1/2.music_seq(collections.deque)，最右 = 下一个要打的
// 按键：两个按钮都是 touch began 判定；按错轨道 = 失败
// 游戏自带自动演奏(is_auto_correct，连击叠满后可开)开着时不点(小鱼同款)
// 2026-11-15 上机：三档难度都能打，连击最高 86
// ============================================================================

#include <algorithm>
#include "AutoQte.h"

namespace AutoMusician {

// ---- 界面配置 ----
static bool  enabled = true;
static float l_x = 514.f,  l_y = 1037.f;     // 左键(轨道1)，屏幕像素；默认值是猜的，上机拖
static float r_x = 2252.f, r_y = 1050.f;     // 右键(轨道2)
static bool  show_touch_point = false;
static bool  dry_run = false;                // 只记日志不点(第一次上机核对时钟用)
static int   lead_ms = 61;
static int   time_jitter_ms = 2;
static int   hold_ms = 25;                   // 音符密，按住要短
static int   jitter_px = 15;

static const int ABILITY_QTE = 1;

// ---- 面板状态 ----
static char g_status[160] = "";
static char g_last[192] = "-";
static char g_params[192] = "";
static std::atomic<int> g_taps{0}, g_missed{0}, g_retap{0};
static std::atomic<int64_t> g_test_request{0};   // 值 = 时间戳*2 + 轨道(0 左 / 1 右)

using AutoQte::now_us;

static double wall_now()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// float/int 原值，不限值域(游戏时间 1 万秒量级)
static bool read_f64(uint64_t vp, double &out)
{
    if (!PyCore::is_obj(vp)) return false;
    uint8_t b[24];
    if (!vm_readv(vp, b, sizeof(b))) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    if (rc <= 0) return false;
    uint64_t tp = 0; memcpy(&tp, b + 8, 8); tp &= 0x00FFFFFFFFFFFFFFULL;
    if (tp == PyRoot::g_float_type) { memcpy(&out, b + 16, 8); return std::isfinite(out); }
    int64_t v = 0;
    if (!PyCore::read_int(vp, v)) return false;
    out = (double)v;
    return true;
}

// 实例属性(dict / split values 都认)。idx 是调用方按"类+属性名"留的序号缓存
static uint64_t attr(uint64_t obj, int64_t &idx, const char *name)
{
    PyCore::InstView v;
    if (!PyCore::inst_view(obj, v)) return 0;
    bool ok = false;
    uint64_t val = PyCore::inst_attr(v, idx, name, ok);
    return ok ? val : 0;
}

static bool attr_f64(uint64_t obj, int64_t &idx, const char *name, double &out)
{
    return read_f64(attr(obj, idx, name), out);
}

static bool attr_int(uint64_t obj, int64_t &idx, const char *name, int64_t &out)
{
    return PyCore::read_int(attr(obj, idx, name), out);
}

// deque 两端：CPython 3.11 dequeobject +0x10 ob_size / +0x18 leftblock / +0x20 rightblock / +0x28 leftindex / +0x30 rightindex
// block = {leftlink, data[64], rightlink}
static bool deque_ends(uint64_t dq, int64_t &n, uint64_t &left, uint64_t &right)
{
    left = right = 0; n = 0;
    if (!PyCore::is_obj(dq)) return false;
    uint8_t b[0x38];
    if (!vm_readv(dq, b, sizeof(b))) return false;
    int64_t rc = 0; memcpy(&rc, b, 8);
    memcpy(&n, b + 0x10, 8);
    if (rc <= 0 || n < 0 || n > 512) return false;
    if (n == 0) return true;
    uint64_t lb = 0, rb = 0; int64_t li = 0, ri = 0;
    memcpy(&lb, b + 0x18, 8); memcpy(&rb, b + 0x20, 8); memcpy(&li, b + 0x28, 8); memcpy(&ri, b + 0x30, 8);
    lb &= 0x00FFFFFFFFFFFFFFULL; rb &= 0x00FFFFFFFFFFFFFFULL;
    if (!PyCore::is_obj(lb) || !PyCore::is_obj(rb) || li < 0 || li >= 64 || ri < 0 || ri >= 64) return false;
    left  = getPtr64(lb + 8 + li * 8);
    right = getPtr64(rb + 8 + ri * 8);
    return PyCore::is_obj(left) && PyCore::is_obj(right);
}

// ---- 一次性(每 500ms 刷新)的参数 ----
struct Params {
    uint64_t item = 0, seq[2] = {0, 0};
    int64_t  mode = -1, difficulty = -1;
    double   length = 0, torrence = 0, valid = 0, offset = 0, move_speed = 1.0;
    bool     auto_correct = false;   // 游戏自带「自动演奏」开着：左右两路游戏自己放行，不用点(小鱼同款)
    // 只用于日志对照(时钟实际用音符校验)：
    //   C_pause = item.room.pause_mgr._abs_game_time − _real_time。2026-11-15 实测比音符钟稳定慢 500ms(room 就是 battle_world)
    //   C_rel   = item.room.relative_game_time − 读到时的 wall(每帧累加的世界钟，小鱼读了这个字段)，看它是不是音符钟
    double   C_pause = NAN, C_rel = NAN;
};

static void read_clock(Params &p)
{
    static thread_local int64_t i_room = -1, i_pm = -1, i_ab = -1, i_rt = -1, i_rel = -1;
    uint64_t room = attr(p.item, i_room, "room");
    uint64_t pm = attr(room, i_pm, "pause_mgr");
    double ab, rt, rel;
    if (pm && attr_f64(pm, i_rt, "_real_time", rt) && attr_f64(pm, i_ab, "_abs_game_time", ab)) p.C_pause = ab - rt;
    if (room && attr_f64(room, i_rel, "relative_game_time", rel)) p.C_rel = rel - wall_now();
}

static bool read_params(Params &p, const char *&why)
{
    static thread_local int64_t i_bind = -1, i_mode = -1, i_diff = -1, i_len = -1, i_tor = -1, i_val = -1, i_off = -1,
                                i_mv = -1, i_pool = -1, i_c1 = -1, i_c2 = -1, i_seq1 = -1, i_seq2 = -1;
    p = Params();
    uint64_t bd = AutoQte::battle_dict(why, false);
    if (!bd) return false;
    uint64_t sb = AutoQte::slot_of(bd, i_bind, "binded_musician_item");
    if (!sb) { why = T("没有作曲家道具(不是作曲家/还没修过机)", "no composer item"); return false; }
    p.item = getPtr64(sb);
    if (!PyCore::is_obj(p.item)) { why = T("作曲家道具为空", "composer item is None"); return false; }
    if (!attr_int(p.item, i_mode, "qte_mode", p.mode)) { why = "qte_mode"; return false; }
    if (!attr_int(p.item, i_diff, "difficulty", p.difficulty)) { why = "difficulty"; return false; }
    int64_t n = 0; uint64_t items = 0;
    if (!PyCore::read_list(attr(p.item, i_len, "music_qte_length_ability"), n, items) || p.difficulty < 0 || p.difficulty >= n
        || !read_f64(getPtr64(items + p.difficulty * 8), p.length)) { why = "music_qte_length_ability"; return false; }
    if (!attr_f64(p.item, i_tor, "client_torrence_ability", p.torrence)) { why = "client_torrence_ability"; return false; }
    attr_f64(p.item, i_val, "valid_torrence_ability", p.valid);
    if (!attr_f64(p.item, i_off, "offset_adjust", p.offset)) p.offset = 0;
    if (!attr_f64(p.item, i_mv, "move_speed", p.move_speed) || p.move_speed <= 0) p.move_speed = 1.0;
    uint64_t pool = attr(p.item, i_pool, "music_pool");
    uint64_t c1 = attr(pool, i_c1, "qte_container_1"), c2 = attr(pool, i_c2, "qte_container_2");
    p.seq[0] = attr(c1, i_seq1, "music_seq");
    p.seq[1] = attr(c2, i_seq2, "music_seq");
    if (!p.seq[0] || !p.seq[1]) { why = T("音符队列读不到", "note queues unreadable"); return false; }
    static thread_local int64_t i_auto = -1;
    p.auto_correct = attr(p.item, i_auto, "is_auto_correct") == PyRoot::g_true;
    read_clock(p);                         // 只做日志对照
    why = "";
    return true;
}

static bool note_begin(uint64_t note, double &bt)
{
    static thread_local int64_t i_bt = -1;
    return attr_f64(note, i_bt, "begin_time", bt);
}

static int tap(uint32_t &rng, int ch, bool exact, float &tx, float &ty, int &hold)
{
    tx = ch == 0 ? l_x : r_x; ty = ch == 0 ? l_y : r_y; hold = hold_ms;
    if (!exact && jitter_px > 0) {
        float ang = AutoQte::frand01(rng) * 6.2831853f, rad = AutoQte::frand01(rng) * (float)jitter_px;
        tx += rad * cosf(ang); ty += rad * sinf(ang);
    }
    if (hold < 8) hold = 8;
    return Touch::InjectTap(tx, ty, hold);
}

static inline void request_test_tap(int ch) { g_test_request.store((now_us() / 1000) * 2 + (ch & 1)); }

static void log_line(const char *s) { printf("[作曲家] %s\n", s); fflush(stdout); }

static void thread_main()
{
    uint32_t rng = (uint32_t)now_us() | 1;
    Params p;
    int64_t params_ms = -100000;
    bool params_ok = false;
    char last_why[96] = "";
    // 已处理的音符(对象地址 + begin_time；对象会被 clone/复用，所以两样一起认)
    struct Done { uint64_t obj; double bt; };
    Done done[16] = {};
    int done_i = 0;
    auto is_done = [&](uint64_t o, double bt) { for (auto &d : done) if (d.obj == o && d.bt == bt) return true; return false; };
    auto mark_done = [&](uint64_t o, double bt) { done[done_i] = {o, bt}; done_i = (done_i + 1) % 16; };
    // 时钟样本：新入队音符(最左)
    uint64_t last_left[2] = {0, 0};
    bool primed[2] = {false, false};      // 开启后第一次看到的最左音符不是"刚入队"的，不算
    // 游戏钟偏移 C(now_game = wall + C)：音符入队那帧 game_now ≈ begin_time，样本 s = begin_time − wall_看到；
    // 取最近 16 个的中位数，离中位数 >50ms 的丢掉(切难度重排音符时会出现 +267/+370/−786ms 的离群样本)。
    // item.room.pause_mgr 直读比音符钟稳定慢 500ms(静态解释不了)，只写日志对照
    const int SAMP = 16;
    double samp[SAMP]; int samp_n = 0, samp_i = 0, samp_total = 0;
    auto C_est = [&]() {
        double t[SAMP]; for (int i = 0; i < samp_n; i++) t[i] = samp[i];
        std::sort(t, t + samp_n);
        return samp_n % 2 ? t[samp_n / 2] : (t[samp_n / 2 - 1] + t[samp_n / 2]) * 0.5;
    };
    bool last_auto = false;
    struct Session { bool active = false; int taps = 0, retaps = 0, gave_up = 0, missed = 0, breaks = 0; int64_t max_combo = 0; };
    Session sess;
    struct Pending { bool active; int ch; uint64_t obj; double bt, qte_p, t_tap; int tries; };
    Pending pend = {false, 0, 0, 0, 0, 0, 0};
    const double RETAP_AFTER = 0.08;      // 点完 80ms 还没被 pop 才算没收到(游戏下一帧才处理 touch began)
    const int    MAX_RETAP = 2;
    uint64_t cur_obj = 0; double cur_bt = 0; int cur_jit = 0;   // 当前瞄准的音符和它的随机量(每个音符抽一次)
    int64_t last_combo = -1;
    static thread_local int64_t i_combo = -1;

    while (true) {
        int64_t req = g_test_request.exchange(0);
        if (req != 0 && now_us() / 1000 - req / 2 < 1000) {
            float tx, ty; int hold; int ch = (int)(req & 1);
            int r = tap(rng, ch, true, tx, ty, hold);
            snprintf(g_last, sizeof(g_last), "test %s %s (%.0f,%.0f)", ch ? "R" : "L", Touch::InjectResultText(r), tx, ty);
            log_line(g_last);
        }

        if (!enabled || !PyRoot::g_ready.load()) {
            snprintf(g_status, sizeof(g_status), "%s", enabled ? T("Python 层未就绪", "Python not ready") : T("未开启", "off"));
            params_ok = false;
            usleep(50000);
            continue;
        }

        int64_t tms = now_us() / 1000;
        if (!params_ok || tms - params_ms > 500) {
            const char *why = "";
            Params np;
            params_ok = read_params(np, why);
            params_ms = tms;
            if (!params_ok) {
                snprintf(g_status, sizeof(g_status), "%s", why);
                if (strcmp(why, last_why) != 0) {
                    snprintf(last_why, sizeof(last_why), "%s", why);
                    char b[128]; snprintf(b, sizeof(b), "读取失败: %s", why); log_line(b);
                }
                usleep(100000);
                continue;
            }
            last_why[0] = 0;
            // 换了道具 = 换局：上一局的钟偏移(相差几千秒)作废。2026-11-15 上机：新局第一个音符拿旧钟算成"晚了 2075187ms"错过
            if (np.item != p.item) { samp_n = samp_i = 0; samp_total = 0; primed[0] = primed[1] = false; }
            // 模式(修机/没修)变化不单独记，进汇总行；只有难度/时长/容差/偏移/换道具才记参数
            bool changed = np.item != p.item || np.difficulty != p.difficulty
                        || np.length != p.length || np.torrence != p.torrence || np.offset != p.offset;
            p = np;
            snprintf(g_params, sizeof(g_params), "模式=%lld 难度=%lld 时长=%.3f 容差=±%.0fms 有效=%.3f 判定线偏移=%.3f",
                     (long long)p.mode, (long long)p.difficulty, p.length, p.torrence * 1000, p.valid, p.offset);
            if (changed) { char b[224]; snprintf(b, sizeof(b), "参数 %s", g_params); log_line(b); }
        }
        // 一段修机(模式进 ABILITY_QTE 到离开)结束时记一行汇总
        if (p.mode == ABILITY_QTE && !sess.active) { sess = Session(); sess.active = true; }
        if (p.mode != ABILITY_QTE && sess.active) {
            sess.active = false;
            if (sess.taps > 0 || sess.missed > 0) {
                char b[224];
                snprintf(b, sizeof(b), "本段修机 难度LV%lld 点击%d 补打%d 补打放弃%d 错过%d 段内连击中断%d 最高连击%lld 提前%d",
                         (long long)p.difficulty + 1, sess.taps, sess.retaps, sess.gave_up, sess.missed, sess.breaks,
                         (long long)sess.max_combo, lead_ms);
                log_line(b);
            }
        }
        if (p.mode != ABILITY_QTE) {
            snprintf(g_status, sizeof(g_status), "%s", T("没在修机(不是修机节奏模式)", "not in fix rhythm mode"));
            usleep(20000);
            continue;
        }

        // ---- 两条轨道最右(下一个要打)的音符 ----
        bool have_clock = samp_n > 0;
        bool clock_reset = false;          // 这一轮里钟被清空重来过：本轮算出的 now_game 是旧钟，不能用
        double C = have_clock ? C_est() : 0.0;
        double now_game = wall_now() + C;
        int best = -1; uint64_t best_obj = 0; double best_bt = 0, best_fire = 0;
        // 自动演奏每轮现读(2026-11-15：原来跟参数 500ms 一刷，关掉后最多半秒不点 → 连击 110 断、两次"错过")
        {
            static thread_local int64_t i_auto_loop = -1;
            p.auto_correct = attr(p.item, i_auto_loop, "is_auto_correct") == PyRoot::g_true;
        }
        uint64_t right_now[2] = {0, 0}; double right_bt[2] = {0, 0};   // 两条轨道这一轮的最右音符(补打用)
        for (int ch = 0; ch < 2; ch++) {
            int64_t n = 0; uint64_t left = 0, right = 0;
            if (!deque_ends(p.seq[ch], n, left, right)) continue;
            if (n == 0) { last_left[ch] = 0; primed[ch] = true; continue; }
            right_now[ch] = right;
            if (!note_begin(right, right_bt[ch])) right_now[ch] = 0;
            // 时钟样本：最左是新入队的音符，入队那帧 game_now ≈ begin_time
            if (left != last_left[ch]) {
                double bt;
                if (primed[ch] && note_begin(left, bt)) {
                    double s = bt - wall_now();
                    // 跟现有估计差超过 3 秒当垃圾(换局/暂停后钟跳了：清空重来)
                    if (samp_n > 0 && fabs(s - C_est()) > 3.0) { samp_n = samp_i = 0; samp_total = 0; clock_reset = true; }
                    if (samp_n >= 5 && fabs(s - C_est()) > 0.05) {
                        char b[160];
                        snprintf(b, sizeof(b), "丢弃离群时钟样本 偏%+.0fms(切难度/重排音符时会出现)", (s - C_est()) * 1000);
                        log_line(b);
                    } else {
                        samp[samp_i] = s; samp_i = (samp_i + 1) % SAMP; if (samp_n < SAMP) samp_n++;
                        samp_total++;
                        if (samp_total == 1) {             // 只记每局第一个样本(对照字段留着，以后热更核对用)
                            double ce = C_est();
                            char b[256];
                            snprintf(b, sizeof(b), "时钟 样本%d 本次=%.3f 中位C=%.3f | 对照 pause_mgr差=%+.0fms relative_game_time差=%+.0fms",
                                     samp_total, s, ce, (p.C_pause - ce) * 1000, (p.C_rel - ce) * 1000);
                            log_line(b);
                        }
                    }
                }
                last_left[ch] = left;
                primed[ch] = true;
            }
            double bt;
            if (!note_begin(right, bt) || is_done(right, bt)) continue;
            double qte_p = bt + p.length - p.offset / p.move_speed;
            if (best < 0 || qte_p < best_fire) { best = ch; best_obj = right; best_bt = bt; best_fire = qte_p; }
        }
        // 游戏自带自动演奏开着(连击叠满后的 is_auto_correct)：左右两路游戏自己判成功，不点(样本照收，钟保持新鲜)
        if (p.auto_correct != last_auto) {
            last_auto = p.auto_correct;
            log_line(p.auto_correct ? "游戏自动演奏开了：左右两路不点，交给游戏" : "游戏自动演奏关了：恢复点击");
        }
        if (p.auto_correct) {
            pend.active = false;
            snprintf(g_status, sizeof(g_status), "%s", T("游戏自动演奏中(不点)", "game auto-play on (not tapping)"));
            usleep(3000);
            continue;
        }
        if (!have_clock || clock_reset) {     // 本轮开头还没钟 / 钟刚重置：下一轮用新钟再算
            snprintf(g_status, sizeof(g_status), "%s", T("修机中，等第一个新音符校准时钟", "fixing, waiting for first note to sync clock"));
            usleep(3000);
            continue;
        }

        // ---- 补打(小鱼同款「兜底补打」)：游戏判了(成功/失败)会立刻把音符 pop 出 music_seq；
        //      点完 RETAP_AFTER 后它还在队列最右 = 这一下游戏没收到(被真手指干扰/注入丢了)，窗口没过就再点一次 ----
        if (pend.active) {
            bool still = right_now[pend.ch] == pend.obj && right_bt[pend.ch] == pend.bt;
            if (!still) {
                pend.active = false;
            } else if (now_game - pend.t_tap >= RETAP_AFTER) {
                double d = now_game - pend.qte_p;
                if (d < p.torrence - 0.02 && pend.tries < MAX_RETAP) {
                    float tx, ty; int hold;
                    int r = tap(rng, pend.ch, false, tx, ty, hold);
                    pend.tries++;
                    pend.t_tap = wall_now() + C;
                    g_retap++; sess.retaps++;
                    char b[192];
                    snprintf(b, sizeof(b), "补打 %s 第%d次 %s (%.0f,%.0f) 相对判定点=%+.1fms(上一下没被游戏吃到)",
                             pend.ch ? "R" : "L", pend.tries, Touch::InjectResultText(r), tx, ty, (pend.t_tap - pend.qte_p) * 1000);
                    log_line(b);
                } else {
                    char b[160];
                    snprintf(b, sizeof(b), "补打放弃 %s 相对判定点=%+.0fms 已补%d次(窗口快过了)", pend.ch ? "R" : "L", d * 1000, pend.tries);
                    log_line(b);
                    pend.active = false;
                    sess.gave_up++;
                }
                continue;
            }
        }
        if (best < 0) {
            snprintf(g_status, sizeof(g_status), "%s", T("修机中，等音符", "fixing, waiting for notes"));
            usleep(3000);
            continue;
        }

        // ---- 到点就点 ----
        if (best_obj != cur_obj || best_bt != cur_bt) { cur_obj = best_obj; cur_bt = best_bt; cur_jit = AutoQte::irand(rng, time_jitter_ms); }
        int jit = cur_jit;
        double fire_game = best_fire - (lead_ms - jit) / 1000.0;
        double left_s = fire_game - now_game;
        if (left_s < -p.torrence * 0.5) {          // 已经过了大半个窗口，放弃这个
            mark_done(best_obj, best_bt);
            g_missed++; sess.missed++;
            char b[160];
            snprintf(b, sizeof(b), "错过 %s 晚了%.0fms(窗口±%.0fms)", best ? "R" : "L", -left_s * 1000, p.torrence * 1000);
            log_line(b);
            continue;
        }
        if (left_s > 0.006) {
            double s = left_s - 0.004;
            usleep(s > 0.003 ? 3000 : (useconds_t)(s * 1e6));
            continue;
        }
        while (wall_now() + C < fire_game) sched_yield();

        double t_tap = wall_now() + C;
        float tx, ty; int hold;
        int r = Touch::INJECT_OK;
        if (dry_run) { tx = best == 0 ? l_x : r_x; ty = best == 0 ? l_y : r_y; hold = 0; }
        else r = tap(rng, best, false, tx, ty, hold);
        mark_done(best_obj, best_bt);
        if (!dry_run) pend = {true, best, best_obj, best_bt, best_fire, t_tap, 0};
        g_taps++; sess.taps++;
        int64_t combo = -1;
        attr_int(p.item, i_combo, "_continue_hit_times", combo);
        // 逐个点击不再记日志(一段修机上百行)；只记注入失败和"只记日志不点"模式
        if (dry_run || r != Touch::INJECT_OK) {
            char b[224];
            snprintf(b, sizeof(b), "%s %s %s (%.0f,%.0f) 相对判定点=%+.1fms 提前%d 随机%+d 窗口±%.0fms",
                     dry_run ? "模拟(没点)" : "注入失败", best ? "R" : "L", Touch::InjectResultText(r), tx, ty,
                     (t_tap - best_fire) * 1000, lead_ms, jit, p.torrence * 1000);
            log_line(b);
        }
        snprintf(g_last, sizeof(g_last), "%s %+.1fms 连击%lld", best ? "R" : "L", (t_tap - best_fire) * 1000, (long long)combo);
        // 段内连击中断：段开头游戏会清连击(不算)，所以从本段第 3 下起才计；服务器回传有滞后，只进汇总不单独记
        if (combo >= 0 && last_combo > 0 && combo < last_combo && sess.taps > 2) sess.breaks++;
        if (combo > sess.max_combo) sess.max_combo = combo;
        if (combo >= 0) last_combo = combo;
        snprintf(g_status, sizeof(g_status), "%s", T("修机中，已点击", "fixing, tapped"));
    }
}

static void start()
{
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(thread_main).detach();
}

static inline const char *status_text() { return g_status[0] ? g_status : T("未开启", "off"); }
static inline const char *last_result() { return g_last; }
static inline const char *params_text() { return g_params[0] ? g_params : "-"; }

} // namespace AutoMusician

#endif
