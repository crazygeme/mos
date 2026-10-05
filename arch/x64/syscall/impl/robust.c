#include <ps/ps.h>
#include <errno.h>
#define ROBUST_WORD uint64_t
#define ROBUST_SIGNED int64_t
#define ROBUST_PREFIX(name) native_##name
#include <arch/abi/robust.h>
