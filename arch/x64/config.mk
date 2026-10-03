# AMD64 kernel with IA-32 compatibility and SMP.
ifeq ($(shell uname),Linux)
CC = gcc
LD = ld
AR = ar
SP = strip
DS = objdump
OC = objcopy
OS := Linux
else ifeq ($(shell uname),Darwin)
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AR = x86_64-elf-ar
SP = x86_64-elf-strip
DS = x86_64-elf-objdump
OC = x86_64-elf-objcopy
OS := Darwin
endif

ARCH_READY := 1
ARCH_CFLAGS := -march=x86-64 -m64 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only -fno-asynchronous-unwind-tables
ARCH_LDFLAGS := -m elf_x86_64
ARCH_LINKER_SCRIPT := $(ARCH_DIR)/link.ld
