MAINPATH = $(shell pwd)
export MAINPATH
include $(MAINPATH)/mos.mk
TARGET	= kernel

SRC_DIRS = src/kernel src/boot src/hw src/test
ASM_DIRS = src/asm

SCRIPTS =  $(MAINPATH)/mos.mk $(MAINPATH)/Makefile
SRCS = 	   $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
ASMS =	   $(foreach dir,$(ASM_DIRS),$(wildcard $(dir)/*.S))
SRC_OBJS = $(patsubst %,$(DST)/%,$(notdir $(SRCS:.c=.c.o)))
ASM_OBJS = $(patsubst %,$(DST)/%,$(notdir $(ASMS:.S=.s.o)))
DEPS =	   $(patsubst %,$(DST)/%,$(notdir $(SRCS:.c=.c.d)))
DEPS +=	   $(patsubst %,$(DST)/%,$(notdir $(ASMS:.S=.s.d)))
LIBS =	   $(DST)/lwext4/libext4.a


vpath %.c $(SRC_DIRS)
vpath %.S $(ASM_DIRS)

all: kernel

$(TARGET): $(ASM_OBJS) $(SRC_OBJS) $(LIBS)
	@echo "LD $@"
	@$(LD) $(LDFLAGS) -o $@ $(SRC_OBJS) $(ASM_OBJS) $(LIBS)
	@cp $(TARGET) $(TARGET).dbg
	@strip $(TARGET)
	@objdump -d $(TARGET).dbg > $(DST)/assemble.s

$(DST)/%.c.o: %.c $(SCRIPTS) | $(DST)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

$(DST)/%.s.o: %.S $(SCRIPTS) | $(DST)
	@echo "CC $<"
	@$(CC) $(ASFLAGS) -MMD -c $< -o $@

$(DST):
	@-mkdir $(DST)

$(DST)/lwext4/libext4.a: $(DST)
	@+make -C 3rdparty

clean:
	@-rm -rf $(DST)
	@-rm $(TARGET)
	@-rm $(TARGET).dbg

rebuild:
	@+make clean
	@+make

-include $(DEPS)

