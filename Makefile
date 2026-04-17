MAINPATH = $(shell pwd)
export MAINPATH
MAKEFLAGS += --no-print-directory
include $(MAINPATH)/build/config.mk
TARGET	= kernel

include $(MAINPATH)/build/helpers.mk

SCRIPTS   = $(MAINPATH)/build/config.mk $(MAINPATH)/build/helpers.mk $(MAINPATH)/Makefile $(SUBDIR_CFLAGS_FILES)
SRCS      = $(shell find src/ -name '*.c')
ASMS      = $(shell find src/ -name '*.S')
TEST_SRCS = $(shell find test/ -name '*.c')
TEST_SCRIPTS = $(shell find test/ -name '*.sh' | sort)

OBJS      = $(patsubst %.c,$(DST)/obj/%.c.o,$(SRCS))
OBJS     += $(patsubst %.S,$(DST)/obj/%.s.o,$(ASMS))
TEST_OBJS = $(patsubst %.c,$(DST)/obj/%.c.o,$(TEST_SRCS))
TEST_OBJS += $(patsubst test/%.sh,$(DST)/obj/generated/test/%.c.o,$(TEST_SCRIPTS))

DEPS      = $(patsubst %.c,$(DST)/obj/%.c.d,$(SRCS))
DEPS     += $(patsubst %.S,$(DST)/obj/%.s.d,$(ASMS))
TEST_DEPS = $(patsubst %.c,$(DST)/obj/%.c.d,$(TEST_SRCS))
TEST_DEPS += $(patsubst test/%.sh,$(DST)/obj/generated/test/%.c.d,$(TEST_SCRIPTS))

GENERATED_TEST_SCRIPT_CS = $(patsubst test/%.sh,$(DST)/generated/test/%.c,$(TEST_SCRIPTS))
BUILD_GENERATED = $(OBJS) $(TEST_OBJS) $(DEPS) $(TEST_DEPS) \
		    $(GENERATED_TEST_SCRIPT_CS) $(LIBS) \
		    $(DST)/kernel $(DST)/kernel.dbg $(DST)/assemble.s \
		    $(DST)/kernel-test $(DST)/kernel-test.dbg \
		    $(DST)/assemble-test.s

LIBS    = $(DST)/obj/third_party/lwext4/libext4.a $(DST)/obj/third_party/lwip/liblwip.a
GENERATED_TEST_CFLAGS-debug = -O0
GENERATED_TEST_CFLAGS-release = -O2

.PHONY: all debug release test test-debug run run-debug run-test run-test-debug \
	clean rebuild third_party format help
.SECONDARY: $(GENERATED_TEST_SCRIPT_CS)

# ── Default build (no test code) ────────────────────────────────────────────

all: $(DST)/kernel
	@:

release:
	@+$(MAKE) BUILD=$(RELEASE) all

debug:
	@+$(MAKE) BUILD=$(DEBUG) all

run: all
	@./run.sh

run-debug:
	@./run.sh debug

# ── Test build (kernel + test/ sources) ─────────────────────────────────────

test: $(DST)/kernel-test
	@:

test-debug:
	@+$(MAKE) BUILD=$(DEBUG) test

run-test: $(DST)/kernel-test
	@./run.sh test

run-test-debug:
	@./run.sh test debug

# ── Link rules ───────────────────────────────────────────────────────────────

$(DST)/kernel: $(OBJS) $(LIBS) $(MAINPATH)/link.ld
	@mkdir -p $(dir $@)
	@echo "LD  $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@cp $(DST)/kernel $(DST)/kernel.dbg
	@$(SP) $(DST)/kernel
	@$(DS) -d $(DST)/kernel.dbg > $(DST)/assemble.s

$(DST)/kernel-test: $(OBJS) $(TEST_OBJS) $(LIBS) $(MAINPATH)/link.ld
	@mkdir -p $(dir $@)
	@echo "LD  $@ (with tests)"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(TEST_OBJS) $(LIBS)
	@cp $(DST)/kernel-test $(DST)/kernel-test.dbg
	@$(SP) $(DST)/kernel-test
	@$(DS) -d $(DST)/kernel-test.dbg > $(DST)/assemble-test.s

# ── Compile rules ────────────────────────────────────────────────────────────

$(DST)/obj/%.c.o: %.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(call dir_cflags,$<) -MMD -MP -c $< -o $@

$(DST)/obj/%.s.o: %.S $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(call dir_cflags,$<) -MMD -MP -c $< -o $@

$(DST)/generated/test/%.c: test/%.sh tools/gen_ktest_scripts.sh
	@mkdir -p $(dir $@)
	@echo "GEN $(subst $(MAINPATH)/,,$@)"
	@tools/gen_ktest_scripts.sh $@ $<

$(DST)/obj/generated/test/%.c.o: $(DST)/generated/test/%.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC  $(subst $(MAINPATH)/,,$<)"
	@$(CC) $(COMMON_CFLAGS) $(GENERATED_TEST_CFLAGS-$(BUILD)) -MMD -MP -c $< -o $@

# ── Third-party ──────────────────────────────────────────────────────────────

$(DST)/obj/third_party/lwext4/libext4.a $(DST)/obj/third_party/lwip/liblwip.a: third_party

third_party: $(SCRIPTS)
	@$(MAKE) -C third_party

# ── Utility ──────────────────────────────────────────────────────────────────

clean:
	@-rm -rf $(DST)

rebuild:
	@-rm -f $(BUILD_GENERATED)
	@+$(MAKE)

format:
	@-find src include test -name "*.c" -o -name "*.h" | xargs clang-format -i

help:
	@echo "Usage:"
	@echo "  make [all|test] [BUILD=release|debug]"
	@echo "  make release"
	@echo "  make debug"
	@echo "  make test-debug"
	@echo "  make run"
	@echo "  make run-debug"
	@echo "  make run-test"
	@echo "  make run-test-debug"
	@echo ""
	@echo "Output directories:"
	@echo "  release -> out/x86/release"
	@echo "  debug   -> out/x86/debug"

-include $(DEPS)
-include $(TEST_DEPS)
