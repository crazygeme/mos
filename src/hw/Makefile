include $(MAINPATH)/mos.mk
SRCS	= $(wildcard *.c)
OBJS	= $(patsubst %.c,%.o,$(SRCS))

all: $(OBJS)
	$(AR) rcs $(DST)/hw.a $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	-rm -f $(OBJS)