ifeq ($(shell uname),Linux)
CC =		gcc
LD =		ld
AR = 		ar
SP =		strip
DS =		objdump
OS =		Linux
else
ifeq ($(shell uname),Darwin)
CC =		i686-elf-gcc
LD =		i686-elf-ld
AR =		i686-elf-ar
SP =		i686-elf-strip
DS =		i686-elf-objdump
OS =		Darwin
else
CC =		i386-elf-gcc
LD =		i386-elf-ld
AR =		i386-elf-ar
SP =		i386-elf-strip
DS =		i386-elf-objdump
OS =		Cygwin
endif
endif

CSTRICT	= 	-Werror=return-type\
		-Werror=uninitialized\
            	-fno-stack-protector\
		-w
CIGNORE	=	-Wno-int-conversion\
		-Wno-incompatible-pointer-types
COMMON_CFLAGS = -fno-pie\
		-fno-builtin\
		-nostdlib\
		-nostdinc\
		-ggdb3\
		-march=i686\
        	-m32\
        	$(CSTRICT)\
		${CIGNORE}\
		-I$(MAINPATH)\
		-I$(MAINPATH)/include\
		-I$(MAINPATH)/third_party/std\
        	-I$(MAINPATH)/third_party/lwext4/include\
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
LDFLAGS	=	-m elf_i386 -T link.ld 
DST     =	$(MAINPATH)/out