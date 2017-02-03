include $(MAINPATH)/mos.mk

SRCS = $(wildcard *.c ./lwext4/src/*.c)
OBJS = $(patsubst %.c,%.o,$(SRCS))

all: $(OBJS)
	$(AR) rcs $(DST)/libext4.a $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	-rm -f $(OBJS)