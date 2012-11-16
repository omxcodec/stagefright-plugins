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

# ffmpeg
include $(CLEAR_VARS)
FFMPEG_SRC_DIR   := $(TOP)/external/ffmpeg
FFMPEG_BUILD_DIR := $(PRODUCT_OUT)/obj/ffmpeg

LOCAL_SRC_FILES := \
	ffmpeg_utils.cpp \
	../../../external/ffmpeg/cmdutils.c

LOCAL_C_INCLUDES := \
	$(TOP)/frameworks/base/include

LOCAL_C_INCLUDES += \
	$(FFMPEG_SRC_DIR) \
	$(FFMPEG_BUILD_DIR)

LOCAL_SHARED_LIBRARIES := \
	libutils

FFMPEG_BUILD_LIBS := \
        -L$(FFMPEG_BUILD_DIR)/libavutil         \
        -L$(FFMPEG_BUILD_DIR)/libavcodec        \
        -L$(FFMPEG_BUILD_DIR)/libswscale        \
        -L$(FFMPEG_BUILD_DIR)/libpostproc       \
        -L$(FFMPEG_BUILD_DIR)/libavformat       \
        -L$(FFMPEG_BUILD_DIR)/libavfilter       \
        -L$(FFMPEG_BUILD_DIR)/libswresample

LOCAL_LDFLAGS += $(FFMPEG_BUILD_LIBS) \
	-lavutil       \
	-lavcodec      \
	-lpostproc     \
	-lavformat     \
	-lavfilter     \
	-lswresample   \
	-lswscale

LOCAL_MODULE:= libffmpeg_utils

LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1

include $(BUILD_SHARED_LIBRARY)
