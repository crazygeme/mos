#ifndef MOS_LIB_BPF_H
#define MOS_LIB_BPF_H
#include <stdint.h>
struct mos_bpf_insn {
	uint16_t code;
	uint8_t jt, jf;
	uint32_t k;
};
struct mos_bpf_filter {
	unsigned count;
	struct mos_bpf_insn instructions[];
};
int mos_bpf_verify(const struct mos_bpf_filter *filter);
uint32_t mos_bpf_run(const struct mos_bpf_filter *filter, const void *data,
		     unsigned length);
#endif
