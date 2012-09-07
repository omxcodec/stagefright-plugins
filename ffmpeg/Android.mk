LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	dummy.c   \
	cmdutils.c

LOCAL_SHARED_LIBRARIES := \
	libnamavutil      \
	libnamavcodec     \
	libnamswscale     \
	libnampostproc    \
	libnamavformat    \
	libnamswresample  \
	libnamavfilter

LOCAL_MODULE:= libnamcmdutils

LOCAL_ARM_MODE := arm

LOCAL_PRELINK_MODULE := false

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

#################################################

include $(CLEAR_VARS)

#LOCAL_WHOLE_STATIC_LIBRARIES := libavformat libavcodec libavfilter libavutil libpostproc libswscale libswresample

LOCAL_SRC_FILES:=	\
	cmdutils.c	\
        ffmpeg.c

LOCAL_SHARED_LIBRARIES := \
	libnamavutil      \
	libnamavcodec     \
	libnamswscale     \
	libnampostproc    \
	libnamavformat    \
	libnamswresample  \
	libnamavfilter

LOCAL_MODULE := ffmpeg-nam

LOCAL_ARM_MODE := arm

LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(call all-makefiles-under,$(LOCAL_PATH))

