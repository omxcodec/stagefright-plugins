LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := libSDL_main
LOCAL_MODULE_TAGS := optional

#$(warning $(LOCAL_PATH))

SDL_PATH := ../SDL
FF_PATH := ../../../../ffmpeg

LOCAL_C_INCLUDES := \
	$(LOCAL_PATH)/../SDL/src/core/android \
	$(LOCAL_PATH)/../SDL/include \
	$(LOCAL_PATH)/../SDL/test \
	$(LOCAL_PATH)/../../../../ffmpeg

#$(warning $(LOCAL_C_INCLUDES))

# Add your application source files here...
#LOCAL_SRC_FILES := $(SDL_PATH)/src/main/android/SDL_android_main.cpp \
	testgles.c

LOCAL_SRC_FILES := $(SDL_PATH)/src/main/android/SDL_android_main.cpp \
	$(FF_PATH)/cmdutils.c \
	ffplay.c

#LOCAL_SRC_FILES := $(SDL_PATH)/src/main/android/SDL_android_main.cpp \
	$(SDL_PATH)/test/testgles.c \
	$(SDL_PATH)/test/common.c

#LOCAL_C_INCLUDES += \
        $(LOCAL_PATH)/../SDL/src/core/android

#LOCAL_SRC_FILES += \
        $(SDL_PATH)/src/core/android/SDL_android.cpp

LOCAL_SHARED_LIBRARIES := libSDL
LOCAL_SHARED_LIBRARIES += libdl libGLESv1_CM libGLESv2 liblog
LOCAL_SHARED_LIBRARIES += \
        libnamavutil       \
        libnamavcodec      \
        libnamswscale      \
        libnampostproc     \
        libnamavformat     \
        libnamswresample   \
        libnamavfilter

include $(BUILD_SHARED_LIBRARY)
