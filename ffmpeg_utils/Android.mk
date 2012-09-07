LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
        ffmpeg_utils.cpp

LOCAL_C_INCLUDES := \
        $(TOP)/nam/ffmpeg \

LOCAL_SHARED_LIBRARIES += \
        libnamavutil       \
	libutils

LOCAL_MODULE:= libffmpeg_utils

LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

include $(BUILD_SHARED_LIBRARY)
