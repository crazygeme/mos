#ifndef _HW_TTY_H_
#define _HW_TTY_H_

#define TTY_MAX_VDEV 12

/* Early VGA/framebuffer initialisation.  Must be called before printk. */
void tty_init(void);

/*
 * tty_default_emit_unsafe - write one character to the TTY without acquiring the lock.
 * ctx is unused; the signature matches the kprint callback type.
 * The caller (printk) must hold the TTY lock via tty_lock_acquire().
 */
void tty_default_emit_unsafe(char c, void *ctx);

/* Acquire / release the TTY spinlock.  Used by printk to batch output. */
void tty_lock_acquire(void);
void tty_lock_release(void);

/* Clear the screen under the TTY lock.  Called by exec before launching
 * the first user process. */
void tty_default_clear(void);

#endif
