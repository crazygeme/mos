#ifndef MOS_SLOTS_H
#define MOS_SLOTS_H

#include <stdint.h>

/* Two bitmap levels support at most 4096 slots without a linear search. */
typedef struct {
	uint64_t free[64], available;
	unsigned capacity;
} slot_pool;

/* value must be nonzero. Use native 32-bit scans to avoid __ctzdi2 on x86. */
static inline unsigned slot_ctz64(uint64_t value)
{
	uint32_t low = (uint32_t)value;
	return low ? __builtin_ctz(low) : 32 + __builtin_ctz((uint32_t)(value >> 32));
}

static inline int slot_take(slot_pool *pool, unsigned capacity)
{
	if (!pool->capacity) {
		pool->capacity = capacity;
		for (unsigned i = 0; i < (capacity + 63) / 64; i++) {
			unsigned bits = capacity - i * 64;
			pool->free[i] = bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
			pool->available |= 1ULL << i;
		}
	}
	if (!pool->available)
		return -1;
	unsigned word = slot_ctz64(pool->available);
	unsigned bit = slot_ctz64(pool->free[word]);
	pool->free[word] &= ~(1ULL << bit);
	if (!pool->free[word])
		pool->available &= ~(1ULL << word);
	return word * 64 + bit;
}

static inline void slot_return(slot_pool *pool, unsigned slot)
{
	pool->free[slot / 64] |= 1ULL << (slot % 64);
	pool->available |= 1ULL << (slot / 64);
}

#endif
