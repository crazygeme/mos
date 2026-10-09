#ifndef _LIB_LOCK_GUARD_H
#define _LIB_LOCK_GUARD_H

/* Every guarded lock places this header at the beginning of its object. */
typedef struct {
	int (*enter)(void *lock, const char *func);
	void (*leave)(void *lock, int state);
} lock_operations_t;

typedef struct {
	const lock_operations_t *operations;
} lock_header_t;

/* Context-bearing policies share the header used by primitive locks. */
typedef struct {
	lock_header_t header;
	void *context;
} scoped_lock_t;

typedef struct {
	void *lock;
	void (*leave)(void *lock, int state);
	int state;
} lock_guard_t;

static inline lock_guard_t
lock_guard_enter(const volatile lock_header_t *header, const char *func)
{
	const lock_operations_t *operations = header->operations;
	void *object = (void *)header;
	int state = operations->enter(object, func);
	return (lock_guard_t){ object, operations->leave, state };
}

static inline void lock_guard_leave(lock_guard_t *guard)
{
	guard->leave(guard->lock, guard->state);
}

#define _LOCK_GUARD_NAME_INNER(id) _lock_guard_##id
#define _LOCK_GUARD_NAME(id) _LOCK_GUARD_NAME_INNER(id)

/* Evaluate the lock pointer once. Guards release in reverse declaration order
 * on every lexical scope exit, including early returns. */
#define LOCK_GUARD(lock)                                             \
	lock_guard_t _LOCK_GUARD_NAME(__COUNTER__)                   \
		__attribute__((cleanup(lock_guard_leave), unused)) = \
			lock_guard_enter(&(lock) -> header, __func__)

#endif
