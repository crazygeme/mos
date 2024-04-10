MAINPATH = $(shell pwd)
export MAINPATH
include $(MAINPATH)/mos.mk
TARGET	= kernel

SRC_DIRS = src/syscall src/boot src/hw  src/int src/fs src/lib src/mm src/ps src/elf src/debug

SCRIPTS =  $(MAINPATH)/mos.mk $(MAINPATH)/Makefile
SRCS = 	   $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
ASMS =	   $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.S))
SRC_OBJS = $(patsubst %,$(DST)/%,$(notdir $(SRCS:.c=.c.o)))
ASM_OBJS = $(patsubst %,$(DST)/%,$(notdir $(ASMS:.S=.s.o)))
DEPS =	   $(patsubst %,$(DST)/%,$(notdir $(SRCS:.c=.c.d)))
DEPS +=	   $(patsubst %,$(DST)/%,$(notdir $(ASMS:.S=.s.d)))
LIBS =	   $(DST)/lwext4/libext4.a
CFLAGS  =  $(COMMON_CFLAGS) -O0
ASFLAGS	=  $(COMMON_CFLAGS) -O0

vpath %.c $(SRC_DIRS)
vpath %.S $(SRC_DIRS)

all: $(DST)/kernel

$(DST)/$(TARGET): $(ASM_OBJS) $(SRC_OBJS) $(LIBS) | $(DST)
	@echo "LD $@"
	@$(LD) $(LDFLAGS) -o $@ $(SRC_OBJS) $(ASM_OBJS) $(LIBS)
	@cp $(DST)/$(TARGET) $(DST)/$(TARGET).dbg
	@strip $(DST)/$(TARGET)
	@objdump -d $(DST)/$(TARGET).dbg > $(DST)/assemble.s

$(DST)/%.c.o: %.c $(SCRIPTS) | $(DST)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

$(DST)/%.s.o: %.S $(SCRIPTS) | $(DST)
	@echo "CC $<"
	@$(CC) $(ASFLAGS) -MMD -c $< -o $@

$(DST):
	@-mkdir -p $(DST)

$(DST)/lwext4/libext4.a: | $(DST)
	@+make -C 3rdparty

clean:
	@-rm -rf $(DST)

rebuild:
	@+make clean
	@+make

-include $(DEPS)

