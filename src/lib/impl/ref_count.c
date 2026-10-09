#include <lib/ref_count.h>

void ref_count_init(ref_count_t *ref, void (*release)(ref_count_t *))
{
	ref->count = 1;
	ref->release = release;
}

void *ref_count_get(void *resource)
{
	ref_count_t *ref = resource;
	if (ref)
		__atomic_fetch_add(&ref->count, 1, __ATOMIC_RELAXED);
	return resource;
}

void ref_count_put(void *resource)
{
	ref_count_t *ref = resource;
	if (ref && __atomic_fetch_sub(&ref->count, 1, __ATOMIC_ACQ_REL) == 1)
		ref->release(ref);
}

unsigned ref_count_read(const void *resource)
{
	const ref_count_t *ref = resource;
	return ref ? __atomic_load_n(&ref->count, __ATOMIC_ACQUIRE) : 0;
}
