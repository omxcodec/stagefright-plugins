LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

FFMPEG_SRC_DIR := $(TOP)/external/ffmpeg

LOCAL_SRC_FILES := \
	SoftFFmpegVideo.cpp

LOCAL_C_INCLUDES += \
	$(LOCAL_PATH)/../../../.. \
	$(TOP)/frameworks/av/media/libstagefright/include \
	$(TOP)/frameworks/native/include/media/openmax \

LOCAL_C_INCLUDES += \
	$(FFMPEG_SRC_DIR) \
	$(FFMPEG_SRC_DIR)/android/include

LOCAL_SHARED_LIBRARIES := \
	libutils          \
	libcutils         \
	libavutil         \
	libavcodec        \
	libswscale        \
	libffmpeg_utils   \
	libstagefright    \
	libstagefright_omx \
	libstagefright_foundation

LOCAL_MODULE := libstagefright_soft_ffmpegvdec
LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1

#ifeq ($(TARGET_ARCH),arm)
#	LOCAL_CFLAGS += -fpermissive
#endif

include $(BUILD_SHARED_LIBRARY)
