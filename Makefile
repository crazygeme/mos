MAINPATH = $(shell pwd)
export MAINPATH
MAKEFLAGS += --no-print-directory
include $(MAINPATH)/mos.mk
TARGET	= kernel

SCRIPTS   = $(MAINPATH)/mos.mk $(MAINPATH)/Makefile
SRCS      = $(shell find src/ -name '*.c')
ASMS      = $(shell find src/ -name '*.S')
TEST_SRCS = $(shell find test/ -name '*.c')

OBJS      = $(patsubst %.c,$(DST)/obj/%.c.o,$(SRCS))
OBJS     += $(patsubst %.S,$(DST)/obj/%.s.o,$(ASMS))
TEST_OBJS = $(patsubst %.c,$(DST)/obj/%.c.o,$(TEST_SRCS))

DEPS      = $(patsubst %.c,$(DST)/obj/%.c.d,$(SRCS))
DEPS     += $(patsubst %.S,$(DST)/obj/%.s.d,$(ASMS))
TEST_DEPS = $(patsubst %.c,$(DST)/obj/%.c.d,$(TEST_SRCS))

LIBS    = $(DST)/obj/third_party/lwext4/libext4.a $(DST)/obj/third_party/lwip/liblwip.a
CFLAGS  = $(COMMON_CFLAGS) -O0

.PHONY: all test run run-test clean rebuild third_party format

# ── Default build (no test code) ────────────────────────────────────────────

all: $(DST)/kernel
	@:

run: all
	@./run.sh

# ── Test build (kernel + test/ sources) ─────────────────────────────────────

test: $(DST)/kernel-test
	@:

run-test: $(DST)/kernel-test
	@./run.sh test

# ── Link rules ───────────────────────────────────────────────────────────────

$(DST)/kernel: $(OBJS) $(LIBS) $(MAINPATH)/link.ld
	@mkdir -p $(dir $@)
	@echo "LD $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@cp $(DST)/kernel $(DST)/kernel.dbg
	@$(SP) $(DST)/kernel
	@$(DS) -d $(DST)/kernel.dbg > $(DST)/assemble.s

$(DST)/kernel-test: $(OBJS) $(TEST_OBJS) $(LIBS) $(MAINPATH)/link.ld
	@mkdir -p $(dir $@)
	@echo "LD $@ (with tests)"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(TEST_OBJS) $(LIBS)
	@cp $(DST)/kernel-test $(DST)/kernel-test.dbg
	@$(SP) $(DST)/kernel-test
	@$(DS) -d $(DST)/kernel-test.dbg > $(DST)/assemble-test.s

# ── Compile rules ────────────────────────────────────────────────────────────

$(DST)/obj/%.c.o: %.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(DST)/obj/%.s.o: %.S $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Third-party ──────────────────────────────────────────────────────────────

$(DST)/obj/third_party/lwext4/libext4.a $(DST)/obj/third_party/lwip/liblwip.a: third_party

third_party: $(SCRIPTS)
	@$(MAKE) -C third_party

# ── Utility ──────────────────────────────────────────────────────────────────

clean:
	@-rm -rf $(DST)

rebuild:
	@+$(MAKE) clean
	@+$(MAKE)

format:
	@-find src include test -name "*.c" -o -name "*.h" | xargs clang-format -i

-include $(DEPS)
-include $(TEST_DEPS)
