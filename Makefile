MAINPATH = $(shell pwd)
export MAINPATH
include $(MAINPATH)/mos.mk
TARGET	= kernel
ASMS	= $(wildcard src/asm/*.S)
SRCS	= $(wildcard src/kernel/*.c)
SRCS	+= $(wildcard src/hw/*.c)
SRCS	+= $(wildcard src/test/*.c)
OBJS	= $(patsubst %.S,%.s.o,$(ASMS))
OBJS	+= $(patsubst %.c,%.c.o,$(SRCS))
LIBS	= $(DST)/libext4.a

all: kernel

$(TARGET): dst $(OBJS) $(LIBS)
	$(LD) $(LDFLAGS) -e 0x100022 -o $(TARGET) $(OBJS) $(LIBS)
	cp $(TARGET) $(TARGET).dbg
	strip $(TARGET)

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


