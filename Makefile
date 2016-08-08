ifeq ($(shell uname),Linux)
CC		= gcc
ASM		= nasm
LD		= ld
OS		= Linux
else
ifeq ($(shell uname),Darwin)
CC		= /opt/local/bin/i386-elf-gcc
ASM 	= /opt/local/bin/nasm
LD		= /opt/local/bin/i386-elf-ld
OS		= Darwin
else
CC      = i386-elf-gcc
ASM     = nasm
LD      = i386-elf-ld
OS      = Cygwin
endif
endif

CSTRICT	= 	-Werror=return-type\
			-Werror=uninitialized\
			-w
CFLAGS	= -ggdb3 -march=i686 -m32 -c $(CSTRICT) -fno-stack-protector -fno-builtin \
	-D__KERNEL__ -D__DEBUG__\
	-I./ -Iinclude
ASFLAGS	= -f elf32
LDFILE	= -m elf_i386 -T link.ld 
LDFLAGS	= $(LDFILE)
TARGET	= kernel
DST		= obj
OBJS	= $(DST)/boot.o \
		  $(DST)/kernel.o \
		  $(DST)/tty.o \
		  $(DST)/klib.o\
		  $(DST)/int.o\
		  $(DST)/interrupt.o\
		  $(DST)/keyboard.o\
		  $(DST)/list.o\
		  $(DST)/dsr.o\
		  $(DST)/mm.o\
		  $(DST)/mmap.o\
		  $(DST)/phymm.o\
		  $(DST)/timer.o\
		  $(DST)/ps.o\
		  $(DST)/lock.o\
		  $(DST)/vfs.o\
		  $(DST)/ext2.o\
		  $(DST)/hdd.o\
		  $(DST)/block.o\
		  $(DST)/mount.o\
		  $(DST)/namespace.o\
		  $(DST)/elf.o\
		  $(DST)/ps0.o\
		  $(DST)/ffs.o\
		  $(DST)/syscall.o\
		  $(DST)/pagefault.o\
		  $(DST)/chardev.o\
		  $(DST)/kbchar.o\
		  $(DST)/console.o\
		  $(DST)/null.o\
		  $(DST)/cache.o\
		  $(DST)/cyclebuf.o\
		  $(DST)/serial.o\
		  $(DST)/pipe.o\
		  $(DST)/rbtree.o\
		  $(DST)/vga.o\
		  $(DST)/pci.o\
		  $(DST)/gui_test.o\
		  $(DST)/gui_core.o\
		  $(DST)/memcpy.o

all: kernel


kernel: dst $(OBJS)
	$(LD) $(LDFLAGS)-e 0x100010 -o $(TARGET).dbg $(OBJS)
	$(LD) $(LDFLAGS) -s -e 0x100010 -o $(TARGET) $(OBJS)

dst:
	-mkdir $(DST)

$(DST)/boot.o: src/boot/kernel.asm
	$(ASM) $(ASFLAGS) src/boot/kernel.asm -o $(DST)/boot.o

$(DST)/int.o: src/int/int.S
	$(CC) $(CFLAGS) src/int/int.S -o $(DST)/int.o

$(DST)/interrupt.o: src/int/int.c include/int.h
	$(CC) $(CFLAGS) src/int/int.c -o $(DST)/interrupt.o 

$(DST)/kernel.o: src/boot/kernel.c
	$(CC) $(CFLAGS) src/boot/kernel.c -o $(DST)/kernel.o

$(DST)/tty.o: src/drivers/tty.c include/tty.h
	$(CC) $(CFLAGS) src/drivers/tty.c -o $(DST)/tty.o

$(DST)/keyboard.o: src/drivers/keyboard.c include/keyboard.h
	$(CC) $(CFLAGS) src/drivers/keyboard.c -o $(DST)/keyboard.o

$(DST)/klib.o: src/lib/klib.c include/klib.h include/tty.h
	$(CC) $(CFLAGS) src/lib/klib.c -o $(DST)/klib.o

$(DST)/list.o: src/lib/list.c include/list.h
	$(CC) $(CFLAGS) src/lib/list.c -o $(DST)/list.o

$(DST)/rbtree.o: src/lib/rbtree.c include/rbtree.h
	$(CC) $(CFLAGS) src/lib/rbtree.c -o $(DST)/rbtree.o

$(DST)/dsr.o: src/int/dsr.c include/dsr.h
	$(CC) $(CFLAGS) src/int/dsr.c -o $(DST)/dsr.o

$(DST)/mm.o: src/mm/mm.c include/mm.h include/multiboot.h include/mmap.h
	$(CC) $(CFLAGS) -o $(DST)/mm.o src/mm/mm.c

$(DST)/mmap.o: src/mm/mmap.c include/mmap.h include/mm.h
	$(CC) $(CFLAGS) -o $(DST)/mmap.o src/mm/mmap.c

$(DST)/phymm.o: src/mm/phymm.c include/phymm.h include/mm.h
	$(CC) $(CFLAGS) -o $(DST)/phymm.o src/mm/phymm.c

$(DST)/timer.o: src/int/timer.c include/timer.h
	$(CC) $(CFLAGS) -o $(DST)/timer.o src/int/timer.c

$(DST)/ps.o: src/ps/ps.c include/ps.h
	$(CC) $(CFLAGS) -o $(DST)/ps.o src/ps/ps.c

$(DST)/lock.o: src/ps/lock.c include/lock.h
	$(CC) $(CFLAGS) -o $(DST)/lock.o src/ps/lock.c


$(DST)/vfs.o: src/fs/vfs.c include/vfs.h
	$(CC) $(CFLAGS) -o $(DST)/vfs.o src/fs/vfs.c

$(DST)/ext2.o: src/fs/ext2.c include/ext2.h
	$(CC) $(CFLAGS) -o $(DST)/ext2.o src/fs/ext2.c

$(DST)/block.o: include/block.h src/drivers/block.c
	$(CC) $(CFLAGS) -o $(DST)/block.o src/drivers/block.c

$(DST)/hdd.o: include/hdd.h src/drivers/hdd.c
	$(CC) $(CFLAGS) -o $(DST)/hdd.o src/drivers/hdd.c

$(DST)/mount.o: src/fs/mount.c include/mount.h
	$(CC) $(CFLAGS) -o $(DST)/mount.o src/fs/mount.c

$(DST)/namespace.o: src/fs/namespace.c include/namespace.h
	$(CC) $(CFLAGS) -o $(DST)/namespace.o src/fs/namespace.c

$(DST)/elf.o: src/ps/elf.c include/elf.h
	$(CC) $(CFLAGS) -o $(DST)/elf.o src/ps/elf.c

$(DST)/ps0.o: src/ps/ps0.c include/ps0.h
	$(CC) $(CFLAGS) -o $(DST)/ps0.o src/ps/ps0.c

$(DST)/ffs.o: src/fs/ffs.c include/ffs.h
	$(CC) $(CFLAGS) -o $(DST)/ffs.o src/fs/ffs.c

$(DST)/syscall.o: src/syscall/syscall.c include/syscall.h
	$(CC) $(CFLAGS) -o $(DST)/syscall.o src/syscall/syscall.c

$(DST)/pagefault.o: src/mm/pagefault.c include/pagefault.h
	$(CC) $(CFLAGS) -o $(DST)/pagefault.o src/mm/pagefault.c

$(DST)/chardev.o: src/drivers/chardev.c include/chardev.h
	$(CC) $(CFLAGS) -o $(DST)/chardev.o src/drivers/chardev.c

$(DST)/kbchar.o: src/fs/kbchar.c include/kbchar.h
	$(CC) $(CFLAGS) -o $(DST)/kbchar.o src/fs/kbchar.c

$(DST)/console.o: src/fs/console.c include/console.h
	$(CC) $(CFLAGS) -o $(DST)/console.o src/fs/console.c

$(DST)/null.o: src/fs/null.c include/null.h
	$(CC) $(CFLAGS) -o $(DST)/null.o src/fs/null.c

$(DST)/pipe.o: src/fs/pipechar.c include/pipechar.h
	$(CC) $(CFLAGS) -o $(DST)/pipe.o src/fs/pipechar.c

$(DST)/cache.o: src/fs/cache.c include/cache.h
	$(CC) $(CFLAGS) -o $(DST)/cache.o src/fs/cache.c

$(DST)/cyclebuf.o: src/lib/cyclebuf.c include/cyclebuf.h
	$(CC) $(CFLAGS) -o $(DST)/cyclebuf.o src/lib/cyclebuf.c

$(DST)/serial.o: src/drivers/serial.c include/serial.h
	$(CC) $(CFLAGS) -o $(DST)/serial.o src/drivers/serial.c

$(DST)/vga.o: src/drivers/vga.c include/vga.h
	$(CC) $(CFLAGS) -o $(DST)/vga.o src/drivers/vga.c

$(DST)/pci.o: src/drivers/pci.c include/pci.h
	$(CC) $(CFLAGS) -o $(DST)/pci.o src/drivers/pci.c

$(DST)/gui_test.o: src/gui/gui_test.c include/gui_test.h
	$(CC) $(CFLAGS) -o $(DST)/gui_test.o src/gui/gui_test.c

$(DST)/gui_core.o: src/gui/gui_core.c include/gui_core.h
	$(CC) $(CFLAGS) -o $(DST)/gui_core.o src/gui/gui_core.c

$(DST)/memcpy.o: src/lib/memcpy.S
	$(CC) $(CFLAGS) -o $(DST)/memcpy.o src/lib/memcpy.S

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
	-rm -rf $(DST)
	-rm $(TARGET)
	-rm $(TARGET).dbg
	-rm user/run


