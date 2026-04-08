#ifndef _HW_TTY_H_
#define _HW_TTY_H_

#define TTY_MAX_VDEV 10

/* Early VGA/framebuffer initialisation.  Must be called before printk. */
void tty_init(void);

/*
 * tty_default_emit_unsafe - write one character to the TTY without acquiring the lock.
 * ctx is unused; the signature matches the kprint callback type.
 * The caller (printk) must hold the TTY lock via tty_lock_acquire().
 */
void tty_default_emit_unsafe(char c, void *ctx);

/* Acquire / release the TTY spinlock.  Used by printk to batch output. */
void tty_lock_acquire(int *saved_irq);
void tty_lock_release(int irq);

/* Clear the screen under the TTY lock.  Called by exec before launching
 * the first user process. */
void tty_default_clear(void);

/* Switch the active virtual terminal to index n (0-9).
 * If TTY n has no process, a /bin/bash is spawned on it. */
void tty_switch(int n);

/* Route a key byte to the active TTY's keyboard buffer.
 * Called from the keyboard driver interrupt handler. */
void tty_active_kb_put(unsigned char c);

/* If the visible VT is in KD_GRAPHICS mode, schedule any driver-specific
 * display update needed to make user-space framebuffer writes visible.
 * The actual hardware flush may be deferred to a safe non-interrupt context. */
void tty_refresh_graphics(void);

/* Allocate a text snapshot of the active TTY for test introspection.
 * The caller owns the returned buffer and must free it with kfree(). */
char *tty_test_snapshot(unsigned *len_out);

#endif
