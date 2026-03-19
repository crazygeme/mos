include $(MAINPATH)/mos.mk

SRC_DIRS = lwext4/src
SRCS = 		$(wildcard *.c $(SRC_DIRS)/*.c)
OBJS = 		$(patsubst %,$(DST)/lwext4/%,$(notdir $(SRCS:.c=.c.o)))
DEPS = 		$(patsubst %.c,%.c.d,$(SRCS))
CFLAGS  =  	$(filter-out -Werror -Wall,$(COMMON_CFLAGS)) -O2 -Wno-pointer-bool-conversion
ASFLAGS	=  	$(filter-out -Werror -Wall,$(COMMON_CFLAGS)) -O2 -Wno-pointer-bool-conversion
SCRIPTS =  	$(MAINPATH)/mos.mk $(MAINPATH)/Makefile Makefile

vpath %.c  $(SRC_DIRS)

all: $(DST)/lwext4/libext4.a
	@:

$(DST)/lwext4/libext4.a: $(OBJS)
	@echo "AR $@"
	@$(AR) rcs $@ $(OBJS)

$(DST)/lwext4/%.c.o: %.c $(SCRIPTS)
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -o $@ -c $<

clean:
	-@rm -f $(OBJS)

-include $(DEPS)