# x86-64 build contract. ARCH_READY will be enabled when the long-mode
# entry, MMU, interrupt, and context-switch backend is available.
ifeq ($(shell uname),Linux)
CC = gcc
LD = ld
AR = ar
SP = strip
DS = objdump
OS := Linux
else ifeq ($(shell uname),Darwin)
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AR = x86_64-elf-ar
SP = x86_64-elf-strip
DS = x86_64-elf-objdump
OS := Darwin
endif

ARCH_READY := 0
ARCH_CFLAGS := -march=x86-64 -m64 -mno-red-zone -mcmodel=kernel
ARCH_LDFLAGS := -m elf_x86_64
ARCH_LINKER_SCRIPT := $(ARCH_DIR)/link.ld
