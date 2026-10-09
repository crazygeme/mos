#ifndef MOS_LIB_REF_COUNT_H
#define MOS_LIB_REF_COUNT_H

/* Embed this as the first member of a shared resource. A held reference is
 * required before acquiring another; the final put invokes release once. */
typedef struct _ref_count ref_count_t;
struct _ref_count {
	unsigned count;
	void (*release)(ref_count_t *);
};

void ref_count_init(ref_count_t *ref, void (*release)(ref_count_t *));
/* get returns the same resource pointer, enabling typed assignment in C. */
void *ref_count_get(void *resource);
void ref_count_put(void *resource);
unsigned ref_count_read(const void *resource);

#endif
