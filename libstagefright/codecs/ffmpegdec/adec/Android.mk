LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
        SoftFFmpegAudio.cpp

LOCAL_C_INCLUDES += \
        $(LOCAL_PATH)/../../../.. \
        frameworks/av/media/libstagefright/include \
        frameworks/native/include/media/openmax \

LOCAL_SHARED_LIBRARIES := \
        libutils          \
        libcutils         \
        libavutil         \
        libavcodec        \
        libswresample     \
        libffmpeg_utils   \
        libstagefright    \
        libstagefright_omx \
        libstagefright_foundation

LOCAL_MODULE := libstagefright_soft_ffmpegadec
LOCAL_MODULE_TAGS := optional

ifeq ($(USES_NAM),true)
    LOCAL_CFLAGS += -DUSES_NAM
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1
#fix DECLARE_ALIGNED
LOCAL_CFLAGS += -D__GNUC__=1

include $(BUILD_SHARED_LIBRARY)
