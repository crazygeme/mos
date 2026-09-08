DEBUG	=	debug
RELEASE	=	release
BUILD	?=	$(RELEASE)
ARCH	?=	x86

SUPPORTED_ARCHES := x86 x64

ifneq ($(filter $(BUILD),$(DEBUG) $(RELEASE)),$(BUILD))
$(error BUILD must be one of: $(DEBUG) $(RELEASE))
endif

ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHES)),)
$(error ARCH must be one of: $(SUPPORTED_ARCHES))
endif

ARCH_DIR := $(MAINPATH)/arch/$(ARCH)
include $(ARCH_DIR)/config.mk

CSTRICT	= 	-fno-stack-protector\
		-Werror\
		-Wall
CIGNORE	=	-Wno-int-conversion\
		-Wno-unused-function
COMMON_CFLAGS = -fno-pie\
		-fno-builtin\
		-nostdlib\
		-nostdinc\
		-g\
		-ggdb3\
		$(ARCH_CFLAGS)\
		$(CSTRICT)\
		$(CIGNORE)\
		-I$(MAINPATH)/include/arch/$(ARCH)\
		-I$(MAINPATH)\
		-I$(MAINPATH)/include\
		-I$(MAINPATH)/third_party/std\
		-I$(MAINPATH)/third_party/lwext4/include\
		-I$(MAINPATH)/third_party/lwip/src/include\
		-DCONFIG_EXT_FEATURE_SET_LVL=2\
		-DCONFIG_JOURNALING_ENABLE=0\
		-DCONFIG_DIR_INDEX_COMB_SORT=1\
		-DCONFIG_HAVE_OWN_ERRNO=1\
		-DCONFIG_DEBUG_PRINTF=0\
		-DCONFIG_DEBUG_ASSERT=0\
		-DCONFIG_HAVE_OWN_ASSERT=1\
		-DCONFIG_HAVE_OWN_OFLAGS=1\
		-DCONFIG_USE_USER_MALLOC=0\
		-DCONFIG_EXT4_BLOCKDEVS_COUNT=16\
		-DCONFIG_EXT4_MOUNTPOINTS_COUNT=16\
		-DCONFIG_BLOCK_DEV_CACHE_SIZE=1024
LDFLAGS = $(ARCH_LDFLAGS) -T $(ARCH_LINKER_SCRIPT)
DST     = $(MAINPATH)/out/$(ARCH)/$(BUILD)
