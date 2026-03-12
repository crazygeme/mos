MAINPATH = $(shell pwd)
export MAINPATH
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

.PHONY: all clean rebuild

all: $(DST)/kernel

run: all
	@./run.sh

$(DST)/$(TARGET): $(OBJS) $(LIBS) | $(DST)
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

$(DST):
	@-mkdir -p $(DST)

$(DST)/lwext4/libext4.a: | $(DST)
	@+$(MAKE) -C third_party

clean:
	@-rm -rf $(DST)

rebuild:
	@+$(MAKE) clean
	@+$(MAKE)

-include $(DEPS)

