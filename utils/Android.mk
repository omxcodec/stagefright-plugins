LOCAL_PATH:= $(call my-dir)

# common
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	common_utils.cpp

LOCAL_SHARED_LIBRARIES := \
	libutils

LOCAL_MODULE:= libcommon_utils

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)


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

include $(BUILD_SHARED_LIBRARY)
