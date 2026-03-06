MAINPATH = $(shell pwd)
export MAINPATH
include $(MAINPATH)/mos.mk
TARGET	= kernel

SRC_DIRS = src/syscall src/boot src/hw src/int src/fs src/lib src/mm src/ps src/elf src/debug

SCRIPTS =  $(MAINPATH)/mos.mk $(MAINPATH)/Makefile
SRCS = 	   $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
ASMS =	   $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.S))
OBJS =	   $(patsubst %,$(DST)/obj/%,$(notdir $(SRCS:.c=.c.o)))
OBJS +=    $(patsubst %,$(DST)/obj/%,$(notdir $(ASMS:.S=.s.o)))
DEPS =	   $(patsubst %,$(DST)/obj/%,$(notdir $(SRCS:.c=.c.d)))
DEPS +=	   $(patsubst %,$(DST)/obj/%,$(notdir $(ASMS:.S=.s.d)))
LIBS =	   $(DST)/lwext4/libext4.a
CFLAGS  =  $(COMMON_CFLAGS) -O0

vpath %.c $(SRC_DIRS)
vpath %.S $(SRC_DIRS)

.PHONY: all clean rebuild

all: $(DST)/kernel
	
run: all
	@./run.sh

$(DST)/$(TARGET): $(OBJS) $(LIBS) | $(DST)
	@echo "LD $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@cp $(DST)/$(TARGET) $(DST)/$(TARGET).dbg
	@strip $(DST)/$(TARGET)
	@objdump -d $(DST)/$(TARGET).dbg > $(DST)/assemble.s

$(DST)/obj/%.c.o: %.c $(SCRIPTS) | $(DST)/obj
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

$(DST)/obj/%.s.o: %.S $(SCRIPTS) | $(DST)/obj
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -c $< -o $@

$(DST):
	@-mkdir -p $(DST)

$(DST)/obj:
	@-mkdir -p $(DST)/obj

$(DST)/lwext4/libext4.a: | $(DST)
	@+$(MAKE) -C 3rdparty

clean:
	@-rm -rf $(DST)

rebuild:
	@+$(MAKE) clean
	@+$(MAKE)

-include $(DEPS)

