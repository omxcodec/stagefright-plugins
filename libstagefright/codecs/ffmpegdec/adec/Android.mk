LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

FFMPEG_SRC_DIR := $(TOP)/external/ffmpeg

LOCAL_SRC_FILES := \
	SoftFFmpegAudio.cpp

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
	libswresample     \
	libffmpeg_utils   \
	libstagefright    \
	libstagefright_omx \
	libstagefright_foundation

LOCAL_MODULE := libstagefright_soft_ffmpegadec
LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

#fix DECLARE_ALIGNED 
#LOCAL_CFLAGS += -D__GNUC__=1

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1

#ifeq ($(TARGET_ARCH),arm)
#	LOCAL_CFLAGS += -fpermissive
#endif

include $(BUILD_SHARED_LIBRARY)
