#pragma once
// ============================================================================
// PyRoot.h —— 内嵌 CPython 的根：sys.modules + 5 个类型/单例，不再写死模块偏移
// ============================================================================
//
// 以前 PyProgress.h / PyGenius.h / PySelf.h 各自写死一份
//   sys.modules 槽 0xA7029E8、PyDict 0xA023BF0、PyLong 0xA026498、PyFloat 0xA0336F0、
//   True 0xA0261A0、False 0xA0261C0
// 这 6 个都是"模块基址 + 绝对偏移"，热更时一整段平移，必废。
//
// 2026-09-23 离线比对 0611 / 0917 两版 libclient.so 的结论(逆向存档\_scripts\pyroot_diff.py)：
//   - CPython 自己一个字节都没改，只是被前面变长的游戏代码整体推后：
//     7 个类型对象(type/dict/int/float/bool/str/module)平移量全部是 +0x28AC0
//   - .so 里有一个独立的 ".PyRuntime" 段(= _PyRuntime)，两版大小同为 0x28B28，
//     20837 个 qword 逐个比对 0 处不同，内部 1101 个指针平移量全部一致
//   - sys.modules 槽就在这个段里，段内偏移 +0xE9B0(主解释器的 modules 字段)
//
// 所以现在这样找：
//   1. 从 /proc/<pid>/maps 拿到 libclient.so 的真实路径，读 ELF 段表找 ".PyRuntime"，
//      槽 = libbase + sh_addr + 0xE9B0。段表在文件尾部、不映射进内存，所以读的是文件
//   2. 槽里的对象必须通过结构校验：ob_type 的 tp_name(类型对象 +0x18，C 字符串) == "dict"，
//      且里面有 "builtins" 和 "game_kernel" 两个键。dict 类型就从这里顺手拿到
//   3. 其余 4 个不推算，直接去 sys.modules["builtins"].md_dict 里按名字取
//      "int" / "float" / "True" / "False" —— 那就是 CPython 定义里的对象本身，
//      不存在 True/False 顺序弄反的问题
//   4. 第 1 步失败(读不到文件、段表被剥掉)才退回旧的写死值，同样要过第 2 步的校验
//
// 仍然依赖的常数只有 CPython 自己的布局(+0xE9B0 / tp_name +0x18 / md_dict +0x10 / dict 布局)，
// 它们随解释器版本变，不随游戏热更变。游戏哪天升级 CPython，这里会校验失败并在状态里报出来，
// 不会悄悄读错。
//
// 解释器在游戏启动后才初始化，所以 ensure() 失败是常态，调用方每轮调一次即可，成功后就是一次原子读。

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <ctime>

namespace PyRoot {

static const uint64_t PYRUNTIME_MODULES_OFF = 0xE9B0;      // _PyRuntime 内主解释器 modules 字段
static const uint64_t LEGACY_MODULES_SLOT       = 0xA7029E8;   // 0917 版写死值，只作兜底

static uintptr_t g_libbase = 0;
static uint64_t  g_elf_slot = 0;                         // 从 ELF 段表算出来的，0 = 没算出来
static char      g_so_path[512] = "";

// 就绪后只读
// atomic 而不是 volatile：volatile 只挡编译器优化，不阻止 ARM64 的乱序。
// 没有它，别的线程可能先看到 g_ready=true、再看到还没写进去的类型指针(全 0)，
// 那一轮读取会白白失败。默认的 seq_cst 在 ARM64 上就是 stlr/ldar，不插 dmb，开销与普通读写相当。
static std::atomic<bool> g_ready{false};
static uint64_t g_modules_slot = 0;                       // *(此) = sys.modules
static uint64_t g_dict_type = 0, g_int_type = 0, g_float_type = 0, g_true = 0, g_false = 0;
static char     g_status[128] = "";   // 空 = 未启动，见 status_text()

static std::mutex g_lock;
static time_t     g_last_try = 0;

// 下限 0x50 跟场景层一致，理由见 PyCore.h 的 is_obj
static inline bool is_obj(uint64_t p) { return p > 0x5000000000ULL && p < 0x8000000000ULL; }

static bool str_equals(uint64_t s, const char *want)          // PyASCIIObject: 长度 +0x10，数据 +0x30
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

// 类型对象的 tp_name(+0x18) 是 C 字符串，不是 PyUnicode
static bool type_name_is(uint64_t tp, const char *want)
{
    if (!is_obj(tp)) return false;
    uint64_t np = getPtr64(tp + 0x18);
    if (!is_obj(np)) return false;
    char buf[16] = {0};
    size_t n = strlen(want);
    if (n + 1 > sizeof(buf) || !vm_readv(np, buf, n + 1)) return false;
    return memcmp(buf, want, n + 1) == 0;             // 连 '\0' 一起比，"dict" 不会误中 "dict_keys"
}

// 在 dict 里按字符串键取值，失败返回 0。布局同 PyProgress.h 的 read_dict()
static uint64_t dict_get(uint64_t d, const char *name)
{
    if (!is_obj(d)) return 0;
    uint64_t keys = getPtr64(d + 0x20);
    if (!is_obj(keys)) return 0;
    uint8_t hdr[32];
    if (!vm_readv(keys, hdr, 32)) return 0;
    uint8_t idxb = hdr[9], kind = hdr[10];
    int64_t nent = 0; memcpy(&nent, hdr + 24, 8);
    if (idxb > 30 || nent < 0 || nent > (1 << 22)) return 0;
    uint64_t ent0 = keys + 32 + ((uint64_t)1 << idxb);
    int stride = (kind == 0) ? 24 : 16, key_off = (kind == 0) ? 8 : 0;
    for (int64_t i = 0; i < nent; i++) {
        uint64_t e = ent0 + i * stride;
        if (str_equals(getPtr64(e + key_off), name)) return getPtr64(e + key_off + 8);
    }
    return 0;
}

// 从 maps 找 libbase 所在那一行的文件路径，再读 ELF 段表里 .PyRuntime 的 sh_addr
static bool read_elf(int pid)
{
    char mp[64];
    snprintf(mp, sizeof(mp), "/proc/%d/maps", pid);
    FILE *fp = fopen(mp, "r");
    if (!fp) return false;
    char line[1024];
    unsigned long elf_off = 0;   // ELF 在被映射文件内的偏移：独立 .so 时为 0；
                                 // 国际服引擎内嵌在 split apk 里,偏移非 0(实测 0x2618000),必须据此定位 ELF
    while (fgets(line, sizeof(line), fp)) {
        unsigned long a = 0;
        if (sscanf(line, "%lx-", &a) != 1 || a != g_libbase) continue;
        sscanf(line, "%*lx-%*lx %*s %lx", &elf_off);
        const char *p = strchr(line, '/');
        if (p) {
            snprintf(g_so_path, sizeof(g_so_path), "%s", p);
            g_so_path[strcspn(g_so_path, "\r\n")] = 0;
        }
        break;
    }
    fclose(fp);
    if (!g_so_path[0]) return false;

    FILE *f = fopen(g_so_path, "rb");
    if (!f) return false;
    bool ok = false;
    unsigned char eh[64];
    if (fseek(f, (long)elf_off, SEEK_SET) == 0 && fread(eh, 1, 64, f) == 64 && memcmp(eh, "\x7f" "ELF", 4) == 0 && eh[4] == 2) {
        uint64_t shoff; uint16_t shentsize, shnum, shstrndx;
        memcpy(&shoff, eh + 0x28, 8);
        memcpy(&shentsize, eh + 0x3A, 2);
        memcpy(&shnum, eh + 0x3C, 2);
        memcpy(&shstrndx, eh + 0x3E, 2);
        static unsigned char sh[256 * 64];
        if (shentsize == 64 && shnum > 0 && shnum <= 256 && shstrndx < shnum &&
            fseek(f, (long)(elf_off + shoff), SEEK_SET) == 0 && fread(sh, 64, shnum, f) == shnum) {
            uint64_t stroff, strsz;
            memcpy(&stroff, sh + shstrndx * 64 + 0x18, 8);
            memcpy(&strsz,  sh + shstrndx * 64 + 0x20, 8);
            static char names[8192];
            if (strsz < sizeof(names) && fseek(f, (long)(elf_off + stroff), SEEK_SET) == 0 &&
                fread(names, 1, strsz, f) == strsz) {
                names[strsz] = 0;
                for (int i = 0; i < shnum; i++) {
                    uint32_t nm; uint64_t addr, size;
                    memcpy(&nm,   sh + i * 64 + 0x00, 4);
                    memcpy(&addr, sh + i * 64 + 0x10, 8);
                    memcpy(&size, sh + i * 64 + 0x20, 8);
                    if (nm < strsz && strcmp(names + nm, ".PyRuntime") == 0 && size > PYRUNTIME_MODULES_OFF) {
                        g_elf_slot = g_libbase + addr + PYRUNTIME_MODULES_OFF;
                        ok = true;
                        break;
                    }
                }
            }
        }
    }
    fclose(f);
    return ok;
}

// 校验一个候选槽，通过就把 6 个值全部填好
static bool validate(uint64_t slot)
{
    uint64_t sm = getPtr64(slot);
    if (!is_obj(sm)) return false;
    uint64_t dt = getPtr64(sm + 8);
    if (!type_name_is(dt, "dict")) return false;
    if (!is_obj(dict_get(sm, "game_kernel"))) return false;
    uint64_t bm = dict_get(sm, "builtins");
    if (!is_obj(bm)) return false;
    uint64_t bd = getPtr64(bm + 0x10);                // module.md_dict
    if (!is_obj(bd)) return false;
    uint64_t int_tp = dict_get(bd, "int"), float_tp = dict_get(bd, "float"), dict_tp = dict_get(bd, "dict");
    uint64_t true_obj = dict_get(bd, "True"), false_obj = dict_get(bd, "False");
    if (dict_tp != dt || !type_name_is(int_tp, "int") || !type_name_is(float_tp, "float")) return false;
    if (!is_obj(true_obj) || !is_obj(false_obj) || true_obj == false_obj) return false;
    if (!type_name_is(getPtr64(true_obj + 8), "bool") || getPtr64(false_obj + 8) != getPtr64(true_obj + 8)) return false;
    g_modules_slot = slot;
    g_dict_type = dt; g_int_type = int_tp; g_float_type = float_tp; g_true = true_obj; g_false = false_obj;
    return true;
}

// 调用方每轮调一次。未就绪时最多每秒真正尝试一次
static bool ensure()
{
    if (g_ready) return true;
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_ready) return true;
    if (g_libbase == 0) return false;
    time_t now = time(nullptr);
    if (now == g_last_try) return false;
    g_last_try = now;

    const char *source = nullptr;
    if (g_elf_slot && validate(g_elf_slot))                           source = "ELF .PyRuntime";
    else if (g_elf_slot != g_libbase + LEGACY_MODULES_SLOT &&
             validate(g_libbase + LEGACY_MODULES_SLOT))                   source = T("旧写死值(兜底)", "legacy hardcoded (fallback)");
    if (!source) {
        if (g_elf_slot) snprintf(g_status, sizeof(g_status), T("等待解释器(ELF槽 +0x%llx)", "waiting for interpreter (ELF slot +0x%llx)"), (unsigned long long)(g_elf_slot - g_libbase));
        else         snprintf(g_status, sizeof(g_status), "%s", T("ELF 没读到，旧值也不对", "ELF not read, legacy value invalid"));
        return false;
    }
    snprintf(g_status, sizeof(g_status), T("就绪 %s 槽+0x%llx", "ready %s slot+0x%llx"), source, (unsigned long long)(g_modules_slot - g_libbase));
    printf("[Py根] %s: 槽=+0x%llx dict=+0x%llx int=+0x%llx float=+0x%llx True=+0x%llx False=+0x%llx\n", source,
           (unsigned long long)(g_modules_slot - g_libbase), (unsigned long long)(g_dict_type - g_libbase),
           (unsigned long long)(g_int_type - g_libbase), (unsigned long long)(g_float_type - g_libbase),
           (unsigned long long)(g_true - g_libbase), (unsigned long long)(g_false - g_libbase));
    fflush(stdout);
    g_ready = true;
    return true;
}

static void start(int pid, uintptr_t libbase)
{
    if (g_libbase != 0) return;
    g_libbase = libbase;
    bool ok = read_elf(pid);
    printf("[Py根] so=%s  .PyRuntime %s  槽=+0x%llx\n", g_so_path[0] ? g_so_path : "(没找到)",
           ok ? "命中" : "未找到", (unsigned long long)(ok ? g_elf_slot - libbase : 0));
    fflush(stdout);
    snprintf(g_status, sizeof(g_status), "%s", T("启动中", "starting"));
}

static const char *status_text() { return g_status[0] ? g_status : T("未启动", "not started"); }

} // namespace PyRoot
