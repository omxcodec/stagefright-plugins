# LOCAL_PATH is one of libavutil, libavcodec, libavformat, or libswscale

#include $(LOCAL_PATH)/../config-$(TARGET_ARCH).mak

OBJS :=
OBJS-yes :=
MMX-OBJS-yes :=
ALL_CPP_FILES :=
ALL_CPP_OBJS :=
CPP_FILES :=
CPP_OBJS :=
FFLIBS :=
FFLIBS-yes :=

include $(LOCAL_PATH)/../config.mak
include $(LOCAL_PATH)/Makefile

# have .s files: have target arch subdir
HAVE_S_FILES := $(wildcard $(LOCAL_PATH)/$(TARGET_ARCH)/*.S)
ifneq ($(HAVE_S_FILES),)
include $(LOCAL_PATH)/$(TARGET_ARCH)/Makefile
#$(warning --- have target arch subdir ---)
endif


# collect objects
OBJS-$(HAVE_MMX) += $(MMX-OBJS-yes)
OBJS += $(OBJS-yes)

FFLIBS += $(FFLIBS-yes)
FFLIBS := $(sort $(FFLIBS))

#$(warning 111 $(FFLIBS) ---)

FFNAME := lib$(NAME)
FFLIBS := $(foreach NAME, $(FFLIBS), lib$(NAME))

#$(warning 222 $(FFLIBS) ---)

FFCFLAGS  = -DHAVE_AV_CONFIG_H -Wno-sign-compare -Wno-switch -Wno-pointer-sign
FFCFLAGS += -DTARGET_CONFIG=\"config-$(TARGET_ARCH).h\"

ALL_S_FILES := $(wildcard $(LOCAL_PATH)/$(TARGET_ARCH)/*.S)
ALL_S_FILES := $(addprefix $(TARGET_ARCH)/, $(notdir $(ALL_S_FILES)))

ALL_CPP_FILES := $(wildcard $(LOCAL_PATH)/*.cpp)
#ALL_CPP_FILES := $(addprefix ., $(notdir $(ALL_CPP_FILES)))
ALL_CPP_FILES := $(notdir $(ALL_CPP_FILES))
#$(warning --- $(ALL_CPP_FILES) ---)

ifneq ($(ALL_S_FILES),)
ALL_S_OBJS := $(patsubst %.S,%.o,$(ALL_S_FILES))
ALL_CPP_OBJS := $(patsubst %.cpp,%.o,$(ALL_CPP_FILES))
C_OBJS := $(filter-out $(ALL_S_OBJS) $(ALL_CPP_OBJS),$(OBJS))
CPP_OBJS := $(filter-out $(ALL_S_OBJS) $(C_OBJS),$(OBJS))
#$(warning --- $(C_OBJS) ---)
#$(warning --- $(CPP_OBJS) ---)
S_OBJS := $(filter $(ALL_S_OBJS),$(OBJS))
else
C_OBJS := $(OBJS)
S_OBJS :=
endif

C_FILES := $(patsubst %.o,%.c,$(C_OBJS))
CPP_FILES := $(patsubst %.o,%.cpp,$(CPP_OBJS))
S_FILES := $(patsubst %.o,%.S,$(S_OBJS))

#$(warning --- $(CPP_FILES) ---)

FFFILES := $(sort $(S_FILES)) $(sort $(C_FILES)) $(sort $(CPP_FILES))

