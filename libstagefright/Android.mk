#
# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
include external/ffmpeg/android/ffmpeg.mk

FFMPEG_SRC_DIR := $(TOP)/external/ffmpeg

LOCAL_SRC_FILES := \
	NamExtractor.cpp

LOCAL_C_INCLUDES := \
	$(JNI_H_INCLUDE) \
	$(TOP)/frameworks/base/include \
	$(TOP)/frameworks/base/include/media/stagefright/openmax \
	$(TOP)/frameworks/base/media/libstagefright

LOCAL_C_INCLUDES += \
	$(TOP)/external/stagefright-plugins \
	$(TOP)/external/stagefright-plugins/libstagefright \

LOCAL_C_INCLUDES += \
	$(FFMPEG_SRC_DIR) \
	$(FFMPEG_SRC_DIR)/android/include

LOCAL_SHARED_LIBRARIES := \
	libutils        \
	libcutils       \
	libstagefright  \
	libstagefright_foundation \
	libFFmpegExtractor

LOCAL_MODULE:= libnamparser

LOCAL_MODULE_TAGS := optional

ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS += -Wno-psabi
endif

LOCAL_CFLAGS += -D__STDC_CONSTANT_MACROS=1

ifeq ($(TARGET_ARCH),arm)
LOCAL_CFLAGS += -fpermissive
endif

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
