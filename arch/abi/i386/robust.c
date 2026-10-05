#include <ps/ps.h>
#include <errno.h>
#define ROBUST_WORD uint32_t
#define ROBUST_SIGNED int32_t
#define ROBUST_PREFIX(name) i386_##name
#include <arch/abi/robust.h>
