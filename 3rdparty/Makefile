ifeq ($(shell uname),Linux)
CC		= gcc
LD		= ld
AR		= ar
OS		= Linux
else
ifeq ($(shell uname),Darwin)
CC		= /opt/local/bin/i386-elf-gcc
LD		= /opt/local/bin/i386-elf-ld
AR		=/opt/local/bin/i386-elf-ar
OS		= Darwin
else
CC      = i386-elf-gcc
LD      = i386-elf-ld
AR	= i386-elf-ar
OS      = Cygwin
endif
endif

CSTRICT	= -Werror=return-type -Werror=uninitialized -w
CFLAGS	= -ggdb3 -march=i686 -m32 -c $(CSTRICT) -fno-stack-protector -fno-builtin 

EXT4_SRC=$(wildcard *.c ./lwext4/src/*.c)
EXT4_OBJ=$(patsubst %.c,%.o,$(EXT4_SRC) )
EXT4_FLAGS=-I./lwext4/include\
	-I ../ -I../include\
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

TARGET=libext4.a
all: $(TARGET) 

libext4.a: $(EXT4_OBJ)
	$(AR) rcs libext4.a $(EXT4_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) $(EXT4_FLAGS) -o $@ $^

clean_ext4:
	-rm $(TARGET)
	-rm $(EXT4_OBJ)

clean: clean_ext4
