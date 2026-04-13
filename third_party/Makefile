include $(MAINPATH)/mos.mk

# ── lwext4 ────────────────────────────────────────────────────────────────

EXT4_DIR      = lwext4/src
EXT4_SRCS     = $(wildcard $(EXT4_DIR)/*.c)
EXT4_OBJS     = $(patsubst $(EXT4_DIR)/%.c,$(DST)/obj/third_party/lwext4/%.c.o,$(EXT4_SRCS))
EXT4_DEPS     = $(patsubst %.c.o,%.c.d,$(EXT4_OBJS))

# ── lwip ──────────────────────────────────────────────────────────────────

LWIPDIR       = $(MAINPATH)/third_party/lwip/src
include $(LWIPDIR)/Filelists.mk

LWIP_SRCS     = $(patsubst $(LWIPDIR)/%,%,$(LWIPNOAPPSFILES))
LWIP_OBJS     = $(patsubst %.c,$(DST)/obj/third_party/lwip/%.c.o,$(LWIP_SRCS))
LWIP_DEPS     = $(patsubst %.c.o,%.c.d,$(LWIP_OBJS))

# ── Compiler flags ────────────────────────────────────────────────────────

LWEXT4_CFLAGS = $(filter-out -Werror -Wall,$(COMMON_CFLAGS)) \
              -Wno-pointer-bool-conversion -O2
ASFLAGS     = $(filter-out -Werror -Wall,$(COMMON_CFLAGS)) \
              -Wno-pointer-bool-conversion
LWIP_CFLAGS = $(filter-out -Werror -Wall,$(COMMON_CFLAGS)) \
              -Wno-address \
              -Wno-unused-variable \
              -Wno-unused-function \
              -Wno-int-conversion -O2

SCRIPTS     = $(MAINPATH)/mos.mk $(MAINPATH)/Makefile Makefile

# ── Targets ───────────────────────────────────────────────────────────────

all: $(DST)/obj/third_party/lwext4/libext4.a $(DST)/obj/third_party/lwip/liblwip.a
	@:

$(DST)/obj/third_party/lwext4/libext4.a: $(EXT4_OBJS)
	@mkdir -p $(dir $@)
	@echo "AR  $@"
	@$(AR) rcs $@ $(EXT4_OBJS)

$(DST)/obj/third_party/lwip/liblwip.a: $(LWIP_OBJS)
	@mkdir -p $(dir $@)
	@echo "AR  $@"
	@$(AR) rcs $@ $(LWIP_OBJS)

# ── Compile rules ─────────────────────────────────────────────────────────

$(DST)/obj/third_party/lwext4/%.c.o: $(EXT4_DIR)/%.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) $(LWEXT4_CFLAGS) -MMD -MP -o $@ -c $<

$(DST)/obj/third_party/lwip/%.c.o: $(LWIPDIR)/%.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC  lwip/$*.c"
	@$(CC) $(LWIP_CFLAGS) -MMD -MP -o $@ -c $<

clean:
	-@rm -f $(EXT4_OBJS) $(LWIP_OBJS)

-include $(EXT4_DEPS)
-include $(LWIP_DEPS)
