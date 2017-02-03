ifeq ($(shell uname),Linux)
CC		= gcc
LD		= ld
OS		= Linux
else
ifeq ($(shell uname),Darwin)
CC		= /opt/local/bin/i386-elf-gcc
LD		= /opt/local/bin/i386-elf-ld
OS		= Darwin
else
CC      = i386-elf-gcc
LD      = i386-elf-ld
OS      = Cygwin
endif
endif

CSTRICT	= 	-Werror=return-type\
		-Werror=uninitialized\
                -fno-stack-protector\
                -fno-builtin\
		-w
CFLAGS	= -ggdb3\
	-march=i686\
        -m32\
        -c\
        $(CSTRICT)\
	-I$(MAINPATH)\
	-I$(MAINPATH)/include\
        -I$(MAINPATH)/3rdparty\
	-I$(MAINPATH)/3rdparty/lwext4\
        -I$(MAINPATH)/3rdparty/lwext4/include\
        -D__DEBUG__\
	-DCONFIG_EXT_FEATURE_SET_LVL=2\
        -DCONFIG_JOURNALING_ENABLE=0\
        -DCONFIG_DIR_INDEX_COMB_SORT=1\
        -DCONFIG_HAVE_OWN_ERRNO=1\
        -DCONFIG_DEBUG_PRINTF=0\
        -DCONFIG_DEBUG_ASSERT=0\
        -DCONFIG_HAVE_OWN_ASSERT=1\
        -DCONFIG_HAVE_OWN_OFLAGS=1\
        -DCONFIG_USE_USER_MALLOC=0\
	-DCONFIG_EXT4_MOUNTPOINTS_COUNT=1

ASFLAGS	= -f elf32
LDFLAGS	= -m elf_i386 -T link.ld 
DST     = $(MAINPATH)/obj
TEST 	= test