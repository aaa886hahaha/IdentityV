#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <linux/input.h>
#include <linux/uinput.h>
#include <vector>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "spinlock.h"

#include "imgui.h"
#include "TouchHelperA.h"
#include "Utils.h"

#define maxE 5
#define maxF 10
#define UNGRAB 0
#define GRAB 1

//TODO 触摸穿透

namespace Touch {
    static struct {
        input_event downEvent[2]{{{}, EV_KEY, BTN_TOUCH,       1}, {{}, EV_KEY, BTN_TOOL_FINGER, 1}};
        input_event event[512]{0};
    } input;

    static Vector2 touch_scale;

    static Vector2 screenSize;

    static std::vector<Device> devices;

    static int nowfd;

    static int orientation = 0;

    static bool initialized = false;

    // 2026-10-22：运行时观察真人 slot 实际发过哪些字段(能力位 EVIOCGBIT 会谎报支持，必须看真人实发)。
    // TypeA 线程置位、InjectTap 读；注入只发真人发过的字段，让字段集与真人一致(本机真人不发 major → 不写)。
    static std::atomic<bool> inject_saw_major{false}, inject_saw_pressure{false}, inject_saw_tool_finger{false};

    static bool readOnly = false;

    static bool otherTouch = false;

    static std::function<void(std::vector<Device> *)> callback;

    static spinlock lock;

    void Upload() {
        static bool isFirstDown = true;
        int tmpCnt = 0, tmpCnt2 = 0;
        for (auto &device: devices) {
            for (auto &finger: device.Finger) {
                if (finger.isDown) {
                    if (tmpCnt2++ > 20) {
                        goto finish;
                    }
                    input.event[tmpCnt].type = EV_ABS;
                    input.event[tmpCnt].code = ABS_X;
                    input.event[tmpCnt].value = (int) finger.pos.x;
                    tmpCnt++;

                    input.event[tmpCnt].type = EV_ABS;
                    input.event[tmpCnt].code = ABS_Y;
                    input.event[tmpCnt].value = (int) finger.pos.y;
                    tmpCnt++;

                    input.event[tmpCnt].type = EV_ABS;
                    input.event[tmpCnt].code = ABS_MT_POSITION_X;
                    input.event[tmpCnt].value = (int) finger.pos.x;
                    tmpCnt++;

                    input.event[tmpCnt].type = EV_ABS;
                    input.event[tmpCnt].code = ABS_MT_POSITION_Y;
                    input.event[tmpCnt].value = (int) finger.pos.y;
                    tmpCnt++;

                    input.event[tmpCnt].type = EV_ABS;
                    input.event[tmpCnt].code = ABS_MT_TRACKING_ID;
                    input.event[tmpCnt].value = finger.id;
                    tmpCnt++;

                    input.event[tmpCnt].type = EV_SYN;
                    input.event[tmpCnt].code = SYN_MT_REPORT;
                    input.event[tmpCnt].value = 0;
                    tmpCnt++;
                }
            }
        }
        finish:
        bool is = false;
        if (tmpCnt == 0) {
            input.event[tmpCnt].type = EV_SYN;
            input.event[tmpCnt].code = SYN_MT_REPORT;
            input.event[tmpCnt].value = 0;
            tmpCnt++;
            if (!isFirstDown) {
                isFirstDown = true;
                input.event[tmpCnt].type = EV_KEY;
                input.event[tmpCnt].code = BTN_TOUCH;
                input.event[tmpCnt].value = 0;
                tmpCnt++;
                input.event[tmpCnt].type = EV_KEY;
                input.event[tmpCnt].code = BTN_TOOL_FINGER;
                input.event[tmpCnt].value = 0;
                tmpCnt++;
            }
        } else {
            is = true;
        }
        input.event[tmpCnt].type = EV_SYN;
        input.event[tmpCnt].code = SYN_REPORT;
        input.event[tmpCnt].value = 0;
        tmpCnt++;

        if (is && isFirstDown) {
            isFirstDown = false;
            write(nowfd, &input, sizeof(struct input_event) * (tmpCnt + 2));
        } else {
            write(nowfd, input.event, sizeof(struct input_event) * tmpCnt);
        }
    }


    /*void *TypeB(void *arg) {
        int i = (int) (long) arg;
        Device &device = devices[i];
        int latest = 0;
        input_event inputEvent[64]{0};

        while (Touch_initialized) {
            auto readSize = (int32_t) read(origfd[i], inputEvent, sizeof(inputEvent));
            if (readSize <= 0 || (readSize % sizeof(input_event)) != 0) {
                continue;
            }
            size_t count = size_t(readSize) / sizeof(input_event);
            for (size_t j = 0; j < count; j++) {
                input_event &ie = inputEvent[j];
                if (latest < 0)
                    latest = 0;
                if (latest >= 10)
                    continue;
                if (ie.code == ABS_MT_TRACKING_ID) {
                    if (ie.value < 0) {
                        Finger[i][latest].isDown = false;
                    } else {
                        Finger[i][latest].isDown = true;
                    }
                    Finger[i][latest].id = (i * 2 + 1) * maxF + ie.value;
                    continue;
                }
                if (ie.code == ABS_MT_POSITION_X) {
                    Finger[i][latest].isDown = true;
                    Finger[i][latest].x = (int) (ie.value * S2TX);
                    continue;
                }
                if (ie.code == ABS_MT_POSITION_Y) {
                    Finger[i][latest].isDown = true;
                    Finger[i][latest].y = (int) (ie.value * S2TY);
                    continue;
                }
                if (ie.code == SYN_MT_REPORT) {
                    latest += 1;
                    continue;
                }
                if (ie.code == SYN_REPORT) {
                    Upload();
                    memset(&Finger[i][0], 0, sizeof(Finger) * 10);
                    latest = -1;
                    continue;
                }
            }
        }
        return nullptr;
    }*/

    static void *TypeA(void *arg) {
        int i = (int) (long) arg;
        Device &device = devices[i];

        int latest = 0;
        input_event inputEvent[64]{0};

        while (initialized) {
            auto readSize = (int32_t) read(device.fd, inputEvent, sizeof(inputEvent));
            if (readSize <= 0 || (readSize % sizeof(input_event)) != 0) {
                continue;
            }
            size_t count = size_t(readSize) / sizeof(input_event);

            lock.lock();
            for (size_t j = 0; j < count; j++) {
                input_event &ie = inputEvent[j];
                if (ie.type == EV_ABS) {
                    if (ie.code == ABS_MT_TOUCH_MAJOR) { inject_saw_major = true; continue; }
                    if (ie.code == ABS_MT_PRESSURE)    { inject_saw_pressure = true; continue; }
                    if (ie.code == ABS_MT_SLOT) {
                        latest = ie.value;
                        continue;
                    }
                    if (ie.code == ABS_MT_TRACKING_ID) {
                        if (ie.value == -1) {
                            device.Finger[latest].isDown = false;
                        } else {
                            device.Finger[latest].id = (i * 2 + 1) * maxF + latest;
                            device.Finger[latest].isDown = true;
                        }
                        continue;
                    }
                    if (ie.code == ABS_MT_POSITION_X) {
                        device.Finger[latest].id = (i * 2 + 1) * maxF + latest;
                        device.Finger[latest].pos.x = (float) ie.value * device.S2TX;
                        continue;
                    }
                    if (ie.code == ABS_MT_POSITION_Y) {
                        device.Finger[latest].id = (i * 2 + 1) * maxF + latest;
                        device.Finger[latest].pos.y = (float) ie.value * device.S2TY;
                        continue;
                    }
                }
                if (ie.type == EV_KEY && ie.code == BTN_TOOL_FINGER) { inject_saw_tool_finger = true; continue; }
                if (ie.code == SYN_REPORT) {
                    if (ImGui::GetCurrentContext() != nullptr) {
                        ImGuiIO &io = ImGui::GetIO();
                        if (device.Finger[latest].isDown) {
                            auto pos = Touch2Screen(device.Finger[latest].pos);
                            io.MousePos = ImVec2(pos.x, pos.y);
                            io.MouseDown[0] = true;
                        } else {
                            io.MouseDown[0] = false;
                        }
                    }

                    if (!readOnly) {
                        if (callback) {
                            callback(&devices);
                        } else {
                            Upload();
                        }
                    }
                    continue;
                }
            }
            lock.unlock();

        }
        return nullptr;
    }

    static bool checkDeviceIsTouch(int fd) {
        uint8_t *bits = NULL;
        ssize_t bits_size = 0;
        int res, j, k;
        bool itmp = false, itmp2 = false, itmp3 = false;
        struct input_absinfo abs{};
        while (true) {
            res = ioctl(fd, EVIOCGBIT(EV_ABS, bits_size), bits);
            if (res < bits_size)
                break;
            bits_size = res + 16;
            bits = (uint8_t *) realloc(bits, bits_size * 2);
        }
        for (j = 0; j < res; j++) {
            for (k = 0; k < 8; k++)
                if (bits[j] & 1 << k && ioctl(fd, EVIOCGABS(j * 8 + k), &abs) == 0) {
                    if (j * 8 + k == ABS_MT_SLOT) {
                        itmp = true;
                        continue;
                    }
                    if (j * 8 + k == ABS_MT_POSITION_X) {
                        itmp2 = true;
                        continue;
                    }
                    if (j * 8 + k == ABS_MT_POSITION_Y) {
                        itmp3 = true;
                        continue;
                    }
                }
        }
        free(bits);
        return itmp && itmp2 && itmp3;
    }

    bool Init(const Vector2 &s, bool p_readOnly) {
        Close();
        devices.clear();
        Vector2 size = s;
        readOnly = p_readOnly;
        if (size.x > size.y) {
            screenSize = size;
        } else {
            screenSize = {size.y, size.x};
        }
        DIR *dir = opendir("/dev/input/");
        if (!dir) {
            return false;
        }

        dirent *ptr = NULL;
        int eventCount = 0;
        while ((ptr = readdir(dir)) != NULL) {
            if (strstr(ptr->d_name, "event"))
                eventCount++;
        }

        char temp[128];
        for (int i = 0; i <= eventCount; i++) {
            sprintf(temp, "/dev/input/event%d", i);
            int fd = open(temp, O_RDWR);
            if (fd < 0) {
                continue;
            }
            if (checkDeviceIsTouch(fd)) {
                Device device{};
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &device.absX) == 0
                    && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &device.absY) == 0) {
                    device.fd = fd;
                    if (!readOnly) {
                        ioctl(fd, EVIOCGRAB, GRAB);
                    }
                    devices.push_back(device);
                }
            } else {
                close(fd);
            }
        }

        if (devices.empty()) {
            puts("获取屏幕驱动失败");
            return false;
        }
        //LOGD("device count: %zu", devices.size());

        int screenX = devices[0].absX.maximum;
        int screenY = devices[0].absY.maximum;

        if (!readOnly) {
            struct uinput_user_dev ui_dev;
            nowfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
            if (nowfd <= 0) {
                return false;
            }

            int string_len = rand() % 10 + 5;
            char string[16] = {};  // rand()%10+5 最大 14，固定大小避免 VLA
            memset(&ui_dev, 0, sizeof(ui_dev));

            genRandomString(string, string_len);
            strncpy(ui_dev.name, string, UINPUT_MAX_NAME_SIZE);

            ui_dev.id.bustype = 0;
            ui_dev.id.vendor = rand() % 10 + 5;
            ui_dev.id.product = rand() % 10 + 5;
            ui_dev.id.version = rand() % 10 + 5;

            ioctl(nowfd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

            ioctl(nowfd, UI_SET_EVBIT, EV_ABS);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_X);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_Y);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
            ioctl(nowfd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
            ioctl(nowfd, UI_SET_EVBIT, EV_SYN);
            ioctl(nowfd, UI_SET_EVBIT, EV_KEY);
            ioctl(nowfd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
            ioctl(nowfd, UI_SET_KEYBIT, BTN_TOUCH);

            genRandomString(string, string_len);
            ioctl(nowfd, UI_SET_PHYS, string);

            int fd = devices[0].fd;
            {
                struct input_id id{};
                if (ioctl(fd, EVIOCGID, &id) == 0) {
                    ui_dev.id.bustype = id.bustype;
                    ui_dev.id.vendor = id.vendor;
                    ui_dev.id.product = id.product;
                    ui_dev.id.version = id.version;
                }
                uint8_t *bits = NULL;
                ssize_t bits_size = 0;
                int res, j, k;
                while (1) {
                    res = ioctl(fd, EVIOCGBIT(EV_KEY, bits_size), bits);
                    if (res < bits_size)
                        break;
                    bits_size = res + 16;
                    bits = (uint8_t *) realloc(bits, bits_size * 2);
                }
                for (j = 0; j < res; j++) {
                    for (k = 0; k < 8; k++)
                        if (bits[j] & 1 << k) {
                            if (j * 8 + k == BTN_TOUCH || j * 8 + k == BTN_TOOL_FINGER)
                                continue;
                            ioctl(nowfd, UI_SET_KEYBIT, j * 8 + k);
                        }
                }
                free(bits);
            }
            ui_dev.absmin[ABS_MT_POSITION_X] = 0;
            ui_dev.absmax[ABS_MT_POSITION_X] = screenX;
            ui_dev.absmin[ABS_MT_POSITION_Y] = 0;
            ui_dev.absmax[ABS_MT_POSITION_Y] = screenY;
            ui_dev.absmin[ABS_X] = 0;
            ui_dev.absmax[ABS_X] = screenX;
            ui_dev.absmin[ABS_Y] = 0;
            ui_dev.absmax[ABS_Y] = screenY;
            ui_dev.absmin[ABS_MT_TRACKING_ID] = 0;
            ui_dev.absmax[ABS_MT_TRACKING_ID] = 65535;
            write(nowfd, &ui_dev, sizeof(ui_dev));

            if (ioctl(nowfd, UI_DEV_CREATE)) {
                return false;
            }
        }
        initialized = true;

        pthread_t t;
        for (int i = 0; i < devices.size(); i++) {
            devices[i].S2TX = (float) screenX / (float) devices[i].absX.maximum;
            devices[i].S2TY = (float) screenY / (float) devices[i].absY.maximum;
            pthread_create(&t, nullptr, TypeA, (void *) (long) i);
        }
        if (size.x > size.y) {
            std::swap(size.x, size.y);
        }
        if (otherTouch) {
            std::swap(size.x, size.y);
        }
        touch_scale.x = (float) screenX / size.x;
        touch_scale.y = (float) screenY / size.y;

        //system("chmod 000 -R /proc/bus/input/*");
        return true;
    }

    void Close() {
        if (initialized) {
            for (auto &device: devices) {
                if (!readOnly)
                    ioctl(device.fd, EVIOCGRAB, UNGRAB);
                close(device.fd);
                device.fd = 0;
            }
            if (nowfd > 0) {
                ioctl(nowfd, UI_DEV_DESTROY);
                close(nowfd);
                nowfd = 0;
            }
            memset(input.event, 0, sizeof(input.event));
            initialized = false;
            devices.clear();
        }
    }

    void Down(float x, float y) {
        lock.lock();
        touchObj &touch = devices[0].Finger[9];
        touch.id = 19;
        touch.pos = Vector2(x, y) * touch_scale;
        touch.isDown = true;
        Upload();
        lock.unlock();
    }

    void Move(touchObj *touch, float x, float y) {
        lock.lock();
        touch->pos = Vector2(x, y) * touch_scale;
        Upload();
        lock.unlock();
    }

    void Move(float x, float y) {
        Down(x, y);
    }

    void Up() {
        lock.lock();
        touchObj &touch = devices[0].Finger[9];
        touch.isDown = false;
        Upload();
        lock.unlock();
    }

    void SetCallBack(const std::function<void(std::vector<Device> *)> &cb) {
        callback = cb;
    }

    Vector2 Touch2Screen(const Vector2 &coord) {
        float x = coord.x, y = coord.y;
        float xt = x / touch_scale.x;
        float yt = y / touch_scale.y;

        if (otherTouch) {
            switch (orientation) {
                case 1:
                    x = xt;
                    y = yt;
                    break;
                case 2:
                    y = yt;
                    x = screenSize.y - xt;
                    break;
                case 3:
                    x = screenSize.y - xt;
                    y = screenSize.x - yt;
                    break;
                default:
                    y = xt;
                    x = screenSize.y - yt;
                    break;
            }
        } else {
            switch (orientation) {
                case 1:
                    x = yt;
                    y = screenSize.y - xt;
                    break;
                case 2:
                    x = screenSize.y - xt;
                    y = screenSize.x - yt;
                    break;
                case 3:
                    y = xt;
                    x = screenSize.x - yt;
                    break;
                default:
                    x = xt;
                    y = yt;
                    break;
            }
        }
        return {x, y};
    }

    // Touch2Screen 的逆运算：屏幕像素(当前方向) -> 触摸屏原生坐标。四个方向逐项对照上面反推，
    // 用同一组 orientation/screenSize/touch_scale，保证菜单点得准的设备上注入也点得准
    Vector2 Screen2Touch(const Vector2 &s) {
        const float L = screenSize.x, S = screenSize.y;   // screenSize 在 Init 里固定成 (长边, 短边)
        float xt, yt;
        if (otherTouch) {
            switch (orientation) {
                case 1:  xt = s.x;     yt = s.y;     break;
                case 2:  xt = S - s.x; yt = s.y;     break;
                case 3:  xt = S - s.x; yt = L - s.y; break;
                default: xt = s.y;     yt = S - s.x; break;
            }
        } else {
            switch (orientation) {
                case 1:  xt = S - s.y; yt = s.x;     break;
                case 2:  xt = S - s.x; yt = L - s.y; break;
                case 3:  xt = s.y;     yt = L - s.x; break;
                default: xt = s.x;     yt = s.y;     break;
            }
        }
        return {xt * touch_scale.x, yt * touch_scale.y};
    }

    // ---- 注入一次点击(自动盖板用) ----
    // 学若辰(sub_4241C8)：不 EVIOCGRAB、不建 uinput 虚拟设备，直接往真实触摸屏写一个**空闲的 MT slot**。
    // 玩家的手指各占各的 slot，注入的手指用另一个，摇杆不会断。没有空闲 slot 就放弃，不抢。
    // slot 切换由内核 input core 统一记账(ABS_MT_SLOT 先暂存、有数据时才下发)，驱动下一帧会自己切回它的 slot，
    // 所以写完不用恢复。代价：驱动若开了 INPUT_MT_DROP_UNUSED，玩家手指在动时驱动下一帧会把我们这个 slot 抬起，
    // 按住时间被截短成一帧(约 8ms)——点击照样成立(按下+抬起都在按钮上)。
    static std::mutex inject_mutex;
    static int inject_slots = 0;             // 0 = 还没探测
    static int inject_trk_max = 65535;
    static bool inject_has_btn_touch = false, inject_has_tool_finger = false, inject_has_major = false, inject_has_pressure = false;
    static int inject_pressure = 0;
    static int inject_trk_seq = 0;

    static bool probe_inject_caps(int fd) {
        input_absinfo a{};
        if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &a) != 0 || a.maximum < 1) return false;
        inject_slots = a.maximum + 1;
        if (inject_slots > 32) inject_slots = 32;
        if (ioctl(fd, EVIOCGABS(ABS_MT_TRACKING_ID), &a) == 0 && a.maximum > 16) inject_trk_max = a.maximum;
        uint8_t keybits[KEY_MAX / 8 + 1]{};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
            inject_has_btn_touch   = keybits[BTN_TOUCH / 8] & (1 << (BTN_TOUCH % 8));
            inject_has_tool_finger = keybits[BTN_TOOL_FINGER / 8] & (1 << (BTN_TOOL_FINGER % 8));
        }
        uint8_t absbits[ABS_MAX / 8 + 1]{};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
            inject_has_major = absbits[ABS_MT_TOUCH_MAJOR / 8] & (1 << (ABS_MT_TOUCH_MAJOR % 8));
            inject_has_pressure = absbits[ABS_MT_PRESSURE / 8] & (1 << (ABS_MT_PRESSURE % 8));
            if (inject_has_pressure && ioctl(fd, EVIOCGABS(ABS_MT_PRESSURE), &a) == 0)
                inject_pressure = a.maximum > 1 ? a.maximum / 2 : 1;
        }
        return true;
    }

    // 读每个 slot 当前的 tracking id(-1 = 空闲)。n = 实际读到的个数
    static bool read_slot_ids(int fd, int *ids, int n) {
        struct { __u32 code; __s32 values[32]; } req{};
        req.code = ABS_MT_TRACKING_ID;
        if (ioctl(fd, EVIOCGMTSLOTS(sizeof(__u32) + sizeof(__s32) * n), &req) < 0) return false;
        for (int i = 0; i < n; i++) ids[i] = req.values[i];
        return true;
    }

    static void put_ev(input_event *ev, int &k, int type, int code, int value) {
        ev[k].type = type; ev[k].code = code; ev[k].value = value; k++;
    }

    const char *InjectResultText(int r) {
        switch (r) {
            case INJECT_OK:          return "completed";
            case INJECT_NOT_READY:   return "not_ready";
            case INJECT_NO_SLOT:     return "no_free_slot";
            case INJECT_BAD_POS:     return "invalid_position";
            case INJECT_DOWN_FAILED: return "down_write_failed";
            case INJECT_UP_FAILED:   return "up_write_failed";
            default:                 return "unknown";
        }
    }

    int InjectTap(float sx, float sy, int hold_ms) {
        std::lock_guard<std::mutex> g(inject_mutex);
        if (!initialized || devices.empty() || devices[0].fd <= 0) return INJECT_NOT_READY;
        const int fd = devices[0].fd;
        if (inject_slots == 0 && !probe_inject_caps(fd)) return INJECT_NOT_READY;

        Vector2 p = Screen2Touch({sx, sy});
        const input_absinfo &ax = devices[0].absX, &ay = devices[0].absY;
        if (!(p.x >= ax.minimum && p.x <= ax.maximum && p.y >= ay.minimum && p.y <= ay.maximum))
            return INJECT_BAD_POS;

        int ids[32];
        if (!read_slot_ids(fd, ids, inject_slots)) return INJECT_NOT_READY;
        int slot = -1, others = 0;
        for (int i = inject_slots - 1; i >= 0; i--) {            // 若辰也是从最高的 slot 往下找
            if (ids[i] < 0) { if (slot < 0) slot = i; }
            else others++;
        }
        if (slot < 0) return INJECT_NO_SLOT;

        // tracking id 取在范围顶部附近，避开驱动从小往上分配的那段
        int trk = inject_trk_max - 1 - (inject_trk_seq++ & 7);
        input_event ev[16]{};
        int k = 0;
        put_ev(ev, k, EV_ABS, ABS_MT_SLOT, slot);
        put_ev(ev, k, EV_ABS, ABS_MT_TRACKING_ID, trk);
        put_ev(ev, k, EV_ABS, ABS_MT_POSITION_X, (int) p.x);
        put_ev(ev, k, EV_ABS, ABS_MT_POSITION_Y, (int) p.y);
        // 只发真人实际发过的字段(inject_saw_*)；能力位 inject_has_* 单独不够，会谎报。本机真人不发 major/pressure → 不写。
        if (inject_has_major && inject_saw_major) put_ev(ev, k, EV_ABS, ABS_MT_TOUCH_MAJOR, 6);
        if (inject_has_pressure && inject_saw_pressure) put_ev(ev, k, EV_ABS, ABS_MT_PRESSURE, inject_pressure);
        if (inject_has_btn_touch && others == 0) {
            put_ev(ev, k, EV_KEY, BTN_TOUCH, 1);
            if (inject_has_tool_finger && inject_saw_tool_finger) put_ev(ev, k, EV_KEY, BTN_TOOL_FINGER, 1);  // 真人按下会配套发，补齐
        }
        put_ev(ev, k, EV_SYN, SYN_REPORT, 0);
        if (write(fd, ev, sizeof(input_event) * k) != (ssize_t) (sizeof(input_event) * k))
            return INJECT_DOWN_FAILED;

        if (hold_ms > 0) usleep(hold_ms * 1000);

        // 抬起前重新看一眼：按住期间玩家可能松开了所有手指，那时 BTN_TOUCH 要跟着归零
        others = 0;
        if (read_slot_ids(fd, ids, inject_slots))
            for (int i = 0; i < inject_slots; i++) if (i != slot && ids[i] >= 0) others++;
        k = 0;
        put_ev(ev, k, EV_ABS, ABS_MT_SLOT, slot);
        put_ev(ev, k, EV_ABS, ABS_MT_TRACKING_ID, -1);
        if (inject_has_btn_touch && others == 0) {
            put_ev(ev, k, EV_KEY, BTN_TOUCH, 0);
            if (inject_has_tool_finger && inject_saw_tool_finger) put_ev(ev, k, EV_KEY, BTN_TOOL_FINGER, 0);
        }
        put_ev(ev, k, EV_SYN, SYN_REPORT, 0);
        // 按下已经写进去了，抬起写失败就会留下一根一直按着的手指(卡键)，所以多试几次
        for (int attempt = 0; attempt < 5; attempt++) {
            if (write(fd, ev, sizeof(input_event) * k) == (ssize_t) (sizeof(input_event) * k))
                return INJECT_OK;
            usleep(2000);
        }
        return INJECT_UP_FAILED;
    }

    Vector2 GetScale() {
        return touch_scale;
    }

    void setOrientation(int o) {
        orientation = o;
    }

    void setOtherTouch(bool p_otherTouch) {
        otherTouch = p_otherTouch;
    }
}