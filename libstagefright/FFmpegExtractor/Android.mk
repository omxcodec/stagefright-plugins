LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
        FFmpegExtractor.cpp

LOCAL_C_INCLUDES := \
	$(JNI_H_INCLUDE) \
	$(TOP)/frameworks/base/include \
	$(TOP)/frameworks/base/include/media/stagefright/openmax \
        $(TOP)/frameworks/base/media/libstagefright

LOCAL_C_INCLUDES += \
        $(TOP)/nam			\
        $(TOP)/nam/ffmpeg 		\
        $(TOP)/nam/libstagefright 	\
        $(TOP)/nam/SDL-1.3/android-project/jni/SDL/include

LOCAL_SHARED_LIBRARIES := \
	libutils          \
	libcutils         \
	libstagefright    \
	libstagefright_foundation

LOCAL_SHARED_LIBRARIES +=  \
        libnamavutil       \
        libnamavcodec      \
        libnamswscale      \
        libnampostproc     \
        libnamavformat     \
        libnamswresample   \
        libnamavfilter     \
        libnamcmdutils     \
	libffmpeg_utils    \
        libSDL

LOCAL_MODULE:= libFFmpegExtractor

LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_DEVICE),maguro)
    LOCAL_CFLAGS += -DUSES_NAM
endif

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

include $(BUILD_SHARED_LIBRARY)
