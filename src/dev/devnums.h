#ifndef SRC_DEV_DEVNUMS_H
#define SRC_DEV_DEVNUMS_H

/*
 * Central device-number registry.
 *
 * Keep character/block device majors and fixed minors here so new nodes do not
 * accidentally reuse an existing number that is already registered elsewhere.
 */

/* /dev/mem */
#define MEM_MAJOR 1 /* /dev/mem: major */
#define MEM_MINOR 1 /* /dev/mem: minor */

/* /dev/null */
#define NULL_MAJOR 1 /* /dev/null: major */
#define NULL_MINOR 3 /* /dev/null: minor */

/* /dev/zero */
#define ZERO_MAJOR 1 /* /dev/zero: major */
#define ZERO_MINOR 5 /* /dev/zero: minor */

/* /dev/random and /dev/urandom */
#define RANDOM_MAJOR 1 /* /dev/random, /dev/urandom: shared major */
#define RANDOM_MINOR 8 /* /dev/random: minor */
#define URANDOM_MINOR 9 /* /dev/urandom: minor */

/* /dev/ptyp* (BSD PTY master) */
#define BSD_PTM_MAJOR 2 /* /dev/ptyp*: major */
#define BSD_PTM_MINOR 2 /* /dev/ptyp0 starts at minor 2 */

/* /dev/ttyp* (BSD PTY slave) */
#define BSD_PTS_MAJOR 3 /* /dev/ttyp*: major */

/* /dev/hda* block devices */
#define HDD_MAJOR 3 /* /dev/hda1, /dev/hda2, ...: block major */

/* /dev/tty1..ttyN virtual consoles */
#define TTY_VC_MAJOR 4 /* /dev/tty1, /dev/tty2, ...: major */

/* /dev/tty0, /dev/console, /dev/tty */
#define TTY_AUX_MAJOR 11 /* /dev/tty0, /dev/console, /dev/tty: shared major */

/* /dev/loop* block devices */
#define LOOP_MAJOR 7 /* /dev/loop0, /dev/loop1, ...: block major */

/* /dev/rtc */
#define RTC_MAJOR 10 /* /dev/rtc: major */
#define RTC_MINOR 135 /* /dev/rtc: minor */

/* /dev/input/mice */
#define INPUT_MOUSE_MAJOR 13 /* /dev/input/mice: major */
#define INPUT_MOUSE_MINOR 63 /* /dev/input/mice: minor */

/* /dev/ptmx (Unix98 PTY master) */
#define UNIX98_PTMX_MAJOR 5 /* /dev/ptmx: major */
#define UNIX98_PTMX_MINOR 2 /* /dev/ptmx: minor */

/* /dev/pts  (Unix98 PTY slave namespace) */
#define UNIX98_PTS_MAJOR 0x88 /* /dev/pts/<n>: major */

#endif
