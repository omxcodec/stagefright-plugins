LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
        FFmpegExtractor.cpp

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../.. \
	frameworks/native/include/media/openmax \
	frameworks/av/include \
	frameworks/av/media/libstagefright

LOCAL_SHARED_LIBRARIES := \
	libutils          \
	libcutils         \
	libavcodec        \
	libavformat       \
	libavutil         \
	libffmpeg_utils   \
	libstagefright    \
	libstagefright_foundation

LOCAL_MODULE:= libFFmpegExtractor

LOCAL_MODULE_TAGS := optional

ifeq ($(USES_NAM),true)
    LOCAL_CFLAGS += -DUSES_NAM
endif

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1 -D__STDINT_LIMITS=1

include $(BUILD_SHARED_LIBRARY)
