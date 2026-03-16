#ifndef _FS_SYSLOG_H
#define _FS_SYSLOG_H

/* Write len bytes of str into the kernel syslog ring buffer.
 * Safe to call from any context (uses a spinlock). */
void syslog_write(const char *str, unsigned len);

/* syslog(2) syscall — kernel ring-buffer interface (klogctl).
 *
 * type actions:
 *   0  CLOSE        — no-op, return 0
 *   1  OPEN         — no-op, return 0
 *   2  READ         — read & consume up to len unread bytes into buf
 *   3  READ_ALL     — copy up to len bytes into buf, do not consume
 *   4  READ_CLEAR   — READ_ALL then clear the ring buffer
 *   5  CLEAR        — discard all buffered data
 *   6  CONSOLE_OFF  — no-op, return 0
 *   7  CONSOLE_ON   — no-op, return 0
 *   8  CONSOLE_LEVEL— no-op, return 0
 *   9  SIZE_UNREAD  — return number of unread bytes
 *  10  SIZE_BUFFER  — return total ring buffer capacity
 */
int sys_syslog(int type, char *buf, int len);

#endif
