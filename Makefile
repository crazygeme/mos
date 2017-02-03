MAINPATH = $(shell pwd)
export MAINPATH
include $(MAINPATH)/mos.mk
TARGET	= kernel
ASMS	= $(wildcard $(MAINPATH)/src/kernel/*.S)
SRCS	= $(wildcard $(MAINPATH)/src/kernel/*.c)
HWS		= $(wildcard $(MAINPATH)/src/hw/*.c)
TESTS	= $(wildcard $(MAINPATH)/src/test/*.c)
OBJS	= $(patsubst %.S,%.s.o,$(ASMS))
OBJS	+= $(patsubst %.c,%.c.o,$(SRCS))
OBJS	+= $(patsubst %.c,%.c.o,$(HWS))
OBJS	+= $(patsubst %.c,%.c.o,$(TESTS))
LIBS	= $(DST)/libext4.a

all: kernel

kernel: dst main.o $(OBJS) $(LIBS)
	$(LD) $(LDFLAGS)-e 0x100010 -o $(TARGET).dbg main.o $(OBJS) $(LIBS)
	$(LD) $(LDFLAGS) -s -e 0x100010 -o $(TARGET) main.o $(OBJS) $(LIBS)

main.o: src/kernel.S
	$(CC) $(CFLAGS) -o main.o src/kernel.S

%.c.o: %.c
	$(CC) $(CFLAGS) -o $@ $^

%.s.o: %.S
	$(CC) $(CFLAGS) -o $@ $^

dst:
	-mkdir $(DST)

$(DST)/libext4.a:
	make -C 3rdparty


clean:
	-find . -name '*.o' -o -name '*.a' | xargs rm -f
	-rm -rf $(DST)
	-rm $(TARGET)
	-rm $(TARGET).dbg


