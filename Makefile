MAINPATH = $(shell pwd)
export MAINPATH
MAKEFLAGS += --no-print-directory
include $(MAINPATH)/mos.mk
TARGET	= kernel

SCRIPTS =  $(MAINPATH)/mos.mk $(MAINPATH)/Makefile
SRCS =	   $(shell find src/ -name '*.c')
ASMS =	   $(shell find src/ -name '*.S')
OBJS =	   $(patsubst %.c,$(DST)/obj/%.c.o,$(SRCS))
OBJS +=    $(patsubst %.S,$(DST)/obj/%.s.o,$(ASMS))
DEPS =	   $(patsubst %.c,$(DST)/obj/%.c.d,$(SRCS))
DEPS +=    $(patsubst %.S,$(DST)/obj/%.s.d,$(ASMS))
LIBS =	   $(DST)/lwext4/libext4.a
CFLAGS  =  $(COMMON_CFLAGS) -O0

.PHONY: all clean rebuild third_party

all: $(DST)/kernel
	@:

run: all
	@./run.sh

$(DST)/$(TARGET): $(OBJS) $(LIBS) $(MAINPATH)/link.ld
	@mkdir -p $(dir $@)
	@echo "LD $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@cp $(DST)/$(TARGET) $(DST)/$(TARGET).dbg
	@$(SP) $(DST)/$(TARGET)
	@$(DS) -d $(DST)/$(TARGET).dbg > $(DST)/assemble.s

$(DST)/obj/%.c.o: %.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(DST)/obj/%.s.o: %.S $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@


$(DST)/lwext4/libext4.a: third_party

third_party: $(SCRIPTS)
	@mkdir -p $(dir $@)
	@$(MAKE) -C third_party

clean:
	@-rm -rf $(DST)

rebuild:
	@+$(MAKE) clean
	@+$(MAKE)

format:
	@-find src include -name "*.c" -o -name "*.h" | xargs clang-format -i

-include $(DEPS)

