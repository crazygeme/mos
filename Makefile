ifeq ($(shell uname),Linux)
CC		= gcc
ASM		= nasm
LD		= ld
OS		= Linux
else
CC		= /opt/local/bin/i386-elf-gcc
ASM 	= /opt/local/bin/nasm
LD		= /opt/local/bin/i386-elf-ld
OS		= Darwin
endif

CSTRICT	= -Werror=return-type -Werror=uninitialized
CFLAGS	= -m32 -c $(CSTRICT) -fno-stack-protector -fno-builtin -DDEBUG_FS
ASFLAGS	= -f elf32
LDFILE	= -m elf_i386 -T link.ld 
LDFLAGS	= $(LDFILE)
TARGET	= kernel
OBJS	= boot.o \
		  kernel.o \
		  tty.o \
		  klib.o\
		  int.o\
		  interrupt.o\
		  keyboard.o\
		  list.o\
		  dsr.o\
		  mm.o\
		  timer.o\
		  ps.o\
		  lock.o\
		  vfs.o\
		  ext2.o\
          hdd.o\
          block.o\
		  mount.o\
		  namespace.o

all: kernel

kernel: $(OBJS)
	$(LD) $(LDFLAGS) -e 0x100010 -o $(TARGET) $(OBJS)

boot.o: kernel.asm
	$(ASM) $(ASFLAGS) kernel.asm -o boot.o

int.o: int.S
	$(CC) $(CFLAGS) int.S -o int.o

interrupt.o: int.c int.h
	$(CC) $(CFLAGS) int.c -o interrupt.o 

kernel.o: kernel.c
	$(CC) $(CFLAGS) kernel.c -o kernel.o

tty.o: tty.c tty.h
	$(CC) $(CFLAGS) tty.c -o tty.o

keyboard.o: keyboard.c keyboard.h
	$(CC) $(CFLAGS) keyboard.c -o keyboard.o

klib.o: klib.c klib.h tty.h
	$(CC) $(CFLAGS) klib.c -o klib.o

list.o: list.c list.h
	$(CC) $(CFLAGS) list.c -o list.o

dsr.o: dsr.c dsr.h
	$(CC) $(CFLAGS) dsr.c -o dsr.o

mm.o: mm.c mm.h multiboot.h
	$(CC) $(CFLAGS) -o mm.o mm.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) -o timer.o timer.c

ps.o: ps.c ps.h
	$(CC) $(CFLAGS) -o ps.o ps.c

lock.o: lock.c lock.h
	$(CC) $(CFLAGS) -o lock.o lock.c


vfs.o: fs/vfs.c fs/vfs.h
	$(CC) $(CFLAGS) -o vfs.o fs/vfs.c

ext2.o: fs/ext2.c fs/ext2.h
	$(CC) $(CFLAGS) -o ext2.o fs/ext2.c

block.o: drivers/block.h drivers/block.c
	$(CC) $(CFLAGS) -o block.o drivers/block.c

hdd.o: drivers/hdd.h drivers/hdd.c
	$(CC) $(CFLAGS) -o hdd.o drivers/hdd.c

mount.o: fs/mount.c fs/mount.h
	$(CC) $(CFLAGS) -o mount.o fs/mount.c

namespace.o: fs/namespace.c fs/namespace.h
	$(CC) $(CFLAGS) -o namespace.o fs/namespace.c

user: user/run.h user/run.c
ifeq ($(OS),Linux)
	cd user && make -f Makefile run
	-mkdir mnt
	sudo losetup -o 1048576 /dev/loop0 rootfs.img
	sudo mount /dev/loop0 mnt
	sudo cp user/run mnt/bin/
	ping -c 5 127.0.0.1 > /dev/null
	sudo umount mnt
	sudo losetup -d /dev/loop0
	rm -rf mnt
else
	cd user && make -f Makefile run
endif

clean:
	find . -name "*.o" -exec rm -f {} \;
	-rm $(TARGET)
	-rm user/run

