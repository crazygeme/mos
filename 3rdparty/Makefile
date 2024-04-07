include $(MAINPATH)/mos.mk

SRC_DIRS = lwext4/src
SRCS = 		$(wildcard *.c $(SRC_DIRS)/*.c)
OBJS = 		$(patsubst %,$(DST)/lwext4/%,$(notdir $(SRCS:.c=.c.o)))
DEPS = 		$(patsubst %.c,%.c.d,$(SRCS))
CFLAGS  =  	$(COMMON_CFLAGS) -O2
ASFLAGS	=  	$(COMMON_CFLAGS) -O2
SCRIPTS =  	$(MAINPATH)/mos.mk $(MAINPATH)/Makefile Makefile

vpath %.c  $(SRC_DIRS)

all: $(OBJS)
	@echo "AR $(DST)/lwext4/libext4.a"
	@$(AR) rcs $(DST)/lwext4/libext4.a $(OBJS)

$(DST)/lwext4:
	@-mkdir -p $(DST)/lwext4

$(DST)/lwext4/%.c.o: %.c $(SCRIPTS) | $(DST)/lwext4
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -o $@ -c $<

clean:
	-rm -f $(OBJS)

-include $(DEPS)