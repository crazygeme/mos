include $(MAINPATH)/mos.mk

# ── lwext4 ────────────────────────────────────────────────────────────────

EXT4_SRC_DIRS = lwext4/src
EXT4_SRCS     = $(wildcard *.c $(EXT4_SRC_DIRS)/*.c)
EXT4_OBJS     = $(patsubst %,$(DST)/lwext4/%,$(notdir $(EXT4_SRCS:.c=.c.o)))
EXT4_DEPS     = $(patsubst %.c,%.c.d,$(EXT4_SRCS))

vpath %.c $(EXT4_SRC_DIRS)

# ── lwip ──────────────────────────────────────────────────────────────────

LWIPDIR       = $(MAINPATH)/third_party/lwip/src
include $(LWIPDIR)/Filelists.mk

LWIP_SRCS     = $(LWIPNOAPPSFILES)
LWIP_OBJS     = $(patsubst $(LWIPDIR)/%.c,$(DST)/lwip/%.c.o,$(LWIP_SRCS))
LWIP_DEPS     = $(patsubst $(DST)/lwip/%.c.o,$(DST)/lwip/%.c.d,$(LWIP_OBJS))

# ── Compiler flags ────────────────────────────────────────────────────────

LWEXT4_CFLAGS = $(filter-out -Werror -Wall,$(COMMON_CFLAGS)) \
              -Wno-pointer-bool-conversion
ASFLAGS     = $(filter-out -Werror -Wall,$(COMMON_CFLAGS)) \
              -Wno-pointer-bool-conversion
LWIP_CFLAGS = $(filter-out -Werror -Wall,$(COMMON_CFLAGS)) \
              -Wno-address \
              -Wno-unused-variable \
              -Wno-unused-function \
              -Wno-int-conversion

SCRIPTS     = $(MAINPATH)/mos.mk $(MAINPATH)/Makefile Makefile

# ── Targets ───────────────────────────────────────────────────────────────

all: $(DST)/lwext4/libext4.a $(DST)/lwip/liblwip.a
	@:

$(DST)/lwext4/libext4.a: $(EXT4_OBJS)
	@echo "AR $@"
	@$(AR) rcs $@ $(EXT4_OBJS)

$(DST)/lwip/liblwip.a: $(LWIP_OBJS)
	@echo "AR $@"
	@$(AR) rcs $@ $(LWIP_OBJS)

# ── Compile rules ─────────────────────────────────────────────────────────

$(DST)/lwext4/%.c.o: %.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(LWEXT4_CFLAGS) -MMD -MP -o $@ -c $<

$(DST)/lwip/%.c.o: $(LWIPDIR)/%.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(LWIP_CFLAGS) -MMD -MP -o $@ -c $<

clean:
	-@rm -f $(EXT4_OBJS) $(LWIP_OBJS)

-include $(EXT4_DEPS)
-include $(LWIP_DEPS)
