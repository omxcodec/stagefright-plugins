LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

include $(LOCAL_PATH)/../av.mk

LOCAL_SRC_FILES := $(FFFILES)

LOCAL_C_INCLUDES := \
	$(LOCAL_PATH) \
        $(LOCAL_PATH)/.. \
	$(TOP)/external/zlib \
	$(TOP)/hardware/libhardware/include \
	$(TOP)/frameworks/base/media/libstagefright \
	$(TOP)/frameworks/base/include/media/stagefright/openmax \
	$(TOP)/frameworks/base/include 

LOCAL_CFLAGS += $(FFCFLAGS)

LOCAL_SHARED_LIBRARIES := \
	$(FFLIBS)         \
        libutils          \
        libcutils         \
        libz

#LOCAL_SHARED_LIBRARIES += \
	libbinder         \
        libmedia          \
        libstagefright

LOCAL_MODULE := libnamavcodec

LOCAL_ARM_MODE := arm

LOCAL_PRELINK_MODULE := false

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
