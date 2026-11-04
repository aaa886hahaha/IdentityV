LOCAL_PATH := $(call my-dir)

# 每次构建可覆盖：产物文件名 + 优化等级随机化，防进程名/文件哈希黑名单
# 未通过 1.bat/2.bat 传参时回退到默认值，保证直接 ndk-build 也能编译
MODULE_NAME ?= qq
BUILD_OPT_LEVEL ?= -O2

include $(CLEAR_VARS)
LOCAL_MODULE := $(MODULE_NAME).sh

LOCAL_CFLAGS := -fvisibility=hidden $(BUILD_OPT_LEVEL)
LOCAL_CPPFLAGS := -std=c++17
LOCAL_CPPFLAGS += -fvisibility=hidden
LOCAL_STRIP_MODE := --strip-unneeded --remove-section=.comment

LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES
LOCAL_CPPFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR
LOCAL_CPPFLAGS += -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES


#引入头文件到全局#
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Android_draw
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Android_Graphics
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Android_my_imgui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Android_touch
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/My_Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/src/res



LOCAL_SRC_FILES := src/main.cpp
LOCAL_SRC_FILES += src/Android_draw/draw_Gui.cpp
# 发布版本: 注入功能已停用, 排除注入相关源码及其依赖的第三方黑盒.so, 不参与编译
# LOCAL_SRC_FILES += src/Android_draw/SoHookIntegration.cpp
# LOCAL_SRC_FILES += src/res/cJSON.c
LOCAL_SRC_FILES += src/Android_draw/AndroidImgui.cpp
LOCAL_SRC_FILES += src/Android_touch/TouchHelperA.cpp
LOCAL_SRC_FILES += src/Android_Graphics/GraphicsManager.cpp
LOCAL_SRC_FILES += src/Android_Graphics/OpenGLGraphics.cpp
LOCAL_SRC_FILES += src/Android_Graphics/VulkanGraphics.cpp 
LOCAL_SRC_FILES += src/Android_Graphics/vulkan_wrapper.cpp
LOCAL_SRC_FILES += src/Android_my_imgui/my_imgui.cpp
LOCAL_SRC_FILES += src/Android_my_imgui/my_imgui_impl_android.cpp
LOCAL_SRC_FILES += src/ImGui/imgui.cpp
LOCAL_SRC_FILES += src/ImGui/imgui_demo.cpp
LOCAL_SRC_FILES += src/ImGui/imgui_draw.cpp
LOCAL_SRC_FILES += src/ImGui/imgui_tables.cpp
LOCAL_SRC_FILES += src/ImGui/imgui_widgets.cpp
LOCAL_SRC_FILES += src/ImGui/backends/imgui_impl_android.cpp
LOCAL_SRC_FILES += src/ImGui/backends/imgui_impl_opengl3.cpp
LOCAL_SRC_FILES += src/ImGui/backends/imgui_impl_vulkan.cpp
LOCAL_SRC_FILES += src/My_Utils/stb_image.cpp
    


LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -ldl
# 蜡像师遮挡用的碰撞资源包(jni/src/res/wax_col.pak，WaxAim.h 里 .incbin 嵌入)按图 zlib 压缩，运行时用系统 libz 解压
LOCAL_LDLIBS += -lz

include $(BUILD_EXECUTABLE) #可执行文件
