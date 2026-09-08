# 32-bit i686 backend. Tool selection lives with the architecture so the
# common build does not accumulate host/target conditionals.
ifeq ($(shell uname),Linux)
CC = gcc
LD = ld
AR = ar
SP = strip
DS = objdump
OS := Linux
else ifeq ($(shell uname),Darwin)
CC = i686-elf-gcc
LD = i686-elf-ld
AR = i686-elf-ar
SP = i686-elf-strip
DS = i686-elf-objdump
OS := Darwin
endif

ARCH_READY := 1
ARCH_CFLAGS := -march=i686 -m32
ARCH_LDFLAGS := -m elf_i386
ARCH_LINKER_SCRIPT := $(ARCH_DIR)/link.ld
