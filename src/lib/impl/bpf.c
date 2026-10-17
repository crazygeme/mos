#include <lib/bpf.h>
#include <lib/klib.h>
#include <errno.h>

static void merge(uint32_t *states, unsigned pc, unsigned mask)
{
	states[pc] = states[pc] & 0x10000 ?
			     0x10000 | ((states[pc] & mask) & 0xffff) :
			     0x10000 | mask;
}
int mos_bpf_verify(const struct mos_bpf_filter *filter)
{
	if (!filter || !filter->count || filter->count > 4096)
		return -EINVAL;
	uint32_t *states = zalloc(filter->count * sizeof(*states));
	if (!states)
		return -ENOMEM;
	states[0] = 0x10000;
	int result = -EINVAL;
	for (unsigned i = 0; i < filter->count; i++) {
		const struct mos_bpf_insn *p = &filter->instructions[i];
		unsigned code = p->code, mask = states[i] & 0xffff;
		int terminal = 0, jump = 0;
		switch (code & 7) {
		case 0:
		case 1:
			if (!(code == 0 || code == 1 || code == 0x20 ||
			      code == 0x28 || code == 0x30 || code == 0x40 ||
			      code == 0x48 || code == 0x50 || code == 0x60 ||
			      code == 0x61 || code == 0x80 || code == 0x81 ||
			      code == 0xb1))
				goto out;
			if ((code == 0x60 || code == 0x61) &&
			    (p->k >= 16 ||
			     (states[i] && !(mask & (1U << p->k)))))
				goto out;
			break;
		case 2:
		case 3:
			if ((code != 2 && code != 3) || p->k >= 16)
				goto out;
			mask |= 1U << p->k;
			break;
		case 4:
			if (code > 0xac || (code & 0xf0) > 0xa0 ||
			    ((code & 0xf0) == 0x80 && code != 0x84))
				goto out;
			if (!(code & 8) &&
			    ((code & 0xf0) == 0x30 || (code & 0xf0) == 0x90) &&
			    !p->k)
				goto out;
			break;
		case 5:
			jump = 1;
			if (code == 5) {
				if (p->k >= filter->count - i - 1)
					goto out;
				if (states[i])
					merge(states, i + 1 + p->k, mask);
			} else {
				if (code > 0x4d || (code & 0xf0) < 0x10 ||
				    p->jt >= filter->count - i - 1 ||
				    p->jf >= filter->count - i - 1)
					goto out;
				if (states[i]) {
					merge(states, i + 1 + p->jt, mask);
					merge(states, i + 1 + p->jf, mask);
				}
			}
			break;
		case 6:
			if (code != 6 && code != 0x16)
				goto out;
			terminal = 1;
			break;
		case 7:
			if (code != 7 && code != 0x87)
				goto out;
			break;
		}
		if (!terminal && !jump) {
			if (i + 1 == filter->count)
				goto out;
			if (states[i])
				merge(states, i + 1, mask);
		}
	}
	result = 0;
out:
	free(states);
	return result;
}
uint32_t mos_bpf_run(const struct mos_bpf_filter *filter, const void *data,
		     unsigned length)
{
	const unsigned char *bytes = data;
	uint32_t a = 0, x = 0, memory[16] = { 0 };
	if (!filter)
		return length;
	for (unsigned pc = 0; pc < filter->count; pc++) {
		const struct mos_bpf_insn *p = &filter->instructions[pc];
		unsigned code = p->code;
		uint32_t value = code & 8 ? x : p->k;
		switch (code & 7) {
		case 0:
		case 1: {
			uint32_t loaded = p->k;
			unsigned mode = code & 0xe0;
			if (mode == 0x60)
				loaded = memory[p->k];
			else if (mode == 0x80)
				loaded = length;
			else if (mode == 0x20 || mode == 0x40 || mode == 0xa0) {
				uint64_t offset = p->k;
				if (mode == 0x40)
					offset += x;
				unsigned size = mode == 0xa0 || (code & 0x18) ==
									0x10 ?
							1 :
						(code & 0x18) == 8 ? 2 :
								     4;
				if (offset > length || size > length - offset)
					return 0;
				loaded = 0;
				for (unsigned i = 0; i < size; i++)
					loaded = (loaded << 8) |
						 bytes[offset + i];
				if (mode == 0xa0)
					loaded = (loaded & 15) << 2;
			}
			if (code & 1)
				x = loaded;
			else
				a = loaded;
			break;
		}
		case 2:
			memory[p->k] = a;
			break;
		case 3:
			memory[p->k] = x;
			break;
		case 4:
			switch (code & 0xf0) {
			case 0:
				a += value;
				break;
			case 0x10:
				a -= value;
				break;
			case 0x20:
				a *= value;
				break;
			case 0x30:
				if (!value)
					return 0;
				a /= value;
				break;
			case 0x40:
				a |= value;
				break;
			case 0x50:
				a &= value;
				break;
			case 0x60:
				a = value >= 32 ? 0 : a << value;
				break;
			case 0x70:
				a = value >= 32 ? 0 : a >> value;
				break;
			case 0x80:
				a = 0U - a;
				break;
			case 0x90:
				if (!value)
					return 0;
				a %= value;
				break;
			case 0xa0:
				a ^= value;
				break;
			}
			break;
		case 5: {
			int take = 0;
			if (code == 5) {
				pc += p->k;
				break;
			}
			switch (code & 0xf0) {
			case 0x10:
				take = a == value;
				break;
			case 0x20:
				take = a > value;
				break;
			case 0x30:
				take = a >= value;
				break;
			case 0x40:
				take = !!(a & value);
				break;
			}
			pc += take ? p->jt : p->jf;
			break;
		}
		case 6:
			return code == 6 ? p->k : a;
		case 7:
			if (code == 7)
				x = a;
			else
				a = x;
			break;
		}
	}
	return 0;
}
