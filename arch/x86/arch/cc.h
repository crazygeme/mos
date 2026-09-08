/*
 * arch/cc.h - lwIP compiler/platform glue for MOS (i686, little-endian).
 *
 * lwip/arch.h includes this file unconditionally; it must define the
 * primitive types and a handful of platform macros required by lwIP.
 */
#ifndef _ARCH_CC_H
#define _ARCH_CC_H

#include <stdint.h> /* uint8_t … uint32_t from third_party/std */
#include <stddef.h> /* size_t, NULL                             */

/* ── Byte order ─────────────────────────────────────────────────────────── */
#define BYTE_ORDER LITTLE_ENDIAN

/* ── Basic types ─────────────────────────────────────────────────────────── */
typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uintptr_t mem_ptr_t;

/* ── Limits (limits.h not available) ────────────────────────────────────── */
#define INT_MAX 0x7fffffff
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX 0xffffffffU
#define LONG_MAX 0x7fffffffL

/* ssize_t: define here; tell lwip not to pull in unistd.h for it */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif
#define SSIZE_MAX INT_MAX
#define LWIP_NO_UNISTD_H 1

/* ── Format strings (inttypes.h not available) ──────────────────────────── */
#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

/* ── Printf / diagnostics ───────────────────────────────────────────────── */
/* No stdio in kernel — suppress diagnostic output. */
#define LWIP_PLATFORM_DIAG(x) \
	do {                  \
	} while (0)
#define LWIP_PLATFORM_ASSERT(x) \
	do {                    \
	} while (0)

/* ── Compiler hints ─────────────────────────────────────────────────────── */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

/* ── Byte-swap: use gcc builtins ─────────────────────────────────────────── */
#define LWIP_PLATFORM_BYTESWAP 1
#define LWIP_PLATFORM_HTONS(x) __builtin_bswap16(x)
#define LWIP_PLATFORM_HTONL(x) __builtin_bswap32(x)

#endif /* _ARCH_CC_H */
