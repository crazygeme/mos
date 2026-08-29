#include <lib/klib.h>
#include <stdint.h>
#include <test/test.h>

extern uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem_p);
extern uint64_t __udivdi3(uint64_t num, uint64_t den);
extern uint64_t __umoddi3(uint64_t num, uint64_t den);

typedef struct _division_case {
	uint64_t num;
	uint64_t den;
	uint64_t quot;
	uint64_t rem;
} division_case;

KTEST(klib, unsigned_64_bit_division)
{
	static const division_case cases[] = {
		{ 0, 1, 0, 0 },
		{ 1, 1, 1, 0 },
		{ 1, 2, 0, 1 },
		{ 1000000, 1000, 1000, 0 },
		{ 0xffffffffULL, 10, 429496729ULL, 5 },
		{ 0x100000000ULL, 0xffffffffULL, 1, 1 },
		{ 0xffffffffffffffffULL, 1, 0xffffffffffffffffULL, 0 },
		{ 0xffffffffffffffffULL, 0x8000000000000000ULL, 1,
		  0x7fffffffffffffffULL },
	};
	unsigned i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint64_t rem = 0;
		uint64_t quot =
			__udivmoddi4(cases[i].num, cases[i].den, &rem);

		EXPECT_EQ(quot, cases[i].quot);
		EXPECT_EQ(rem, cases[i].rem);
		EXPECT_EQ(__udivdi3(cases[i].num, cases[i].den),
			  cases[i].quot);
		EXPECT_EQ(__umoddi3(cases[i].num, cases[i].den), cases[i].rem);
	}
	return 0;
}
