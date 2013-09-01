LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

LOCAL_SRC_FILES := \
	ffmpeg_source.cpp \
	ffmpeg_utils.cpp \
	ffmpeg_cmdutils.c

LOCAL_SHARED_LIBRARIES := \
	libavcodec \
	libavformat \
	libavutil \
	libutils \
	libcutils \

LOCAL_MODULE := libffmpeg_utils

LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1

ifeq ($(TARGET_ARCH),arm)
	LOCAL_CFLAGS += -fpermissive
endif

include $(BUILD_SHARED_LIBRARY)
