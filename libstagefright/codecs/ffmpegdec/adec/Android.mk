LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

FFMPEG_SRC_DIR   := $(TOP)/external/ffmpeg
FFMPEG_BUILD_DIR := $(PRODUCT_OUT)/obj/ffmpeg

LOCAL_SRC_FILES := \
        SoftFFmpegAudio.cpp

LOCAL_C_INCLUDES := \
        frameworks/base/media/libstagefright/include \
        frameworks/base/include/media/stagefright/openmax

LOCAL_C_INCLUDES += \
	$(TOP)/external/stagefright-plugins \
	$(TOP)/external/stagefright-plugins/libstagefright \

LOCAL_C_INCLUDES += \
	$(FFMPEG_SRC_DIR) \
	$(FFMPEG_BUILD_DIR)

LOCAL_SHARED_LIBRARIES := \
        libutils          \
        libcutils         \
        libstagefright    \
	libstagefright_omx \
        libstagefright_foundation

LOCAL_SHARED_LIBRARIES +=  \
        libcommon_utils    \
        libffmpeg_utils

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

LOCAL_MODULE := libstagefright_soft_ffmpegadec
LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_DEVICE),maguro)
    LOCAL_CFLAGS += -DUSES_NAM
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1
#fix DECLARE_ALIGNED
LOCAL_CFLAGS += -D__GNUC__=1

include $(BUILD_SHARED_LIBRARY)
