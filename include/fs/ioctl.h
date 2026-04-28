#ifndef _FS_IOCTL_H
#define _FS_IOCTL_H
// from linux

/* 0x54 is just a magic number to make these relatively uniqe ('T') */

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TCGETA 0x5405
#define TCSETA 0x5406
#define TCSETAW 0x5407
#define TCSETAF 0x5408
#define TCSBRK 0x5409
#define TCXONC 0x540A
#define TCFLSH 0x540B
#define TIOCEXCL 0x540C
#define TIOCNXCL 0x540D
#define TIOCSCTTY 0x540E
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCOUTQ 0x5411
#define TIOCSTI 0x5412
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCMGET 0x5415
#define TIOCMBIS 0x5416
#define TIOCMBIC 0x5417
#define TIOCMSET 0x5418
#define TIOCGSOFTCAR 0x5419
#define TIOCSSOFTCAR 0x541A
#define FIONREAD 0x541B
#define TIOCINQ FIONREAD
#define TIOCLINUX 0x541C
#define TIOCCONS 0x541D
#define TIOCGSERIAL 0x541E
#define TIOCSSERIAL 0x541F
#define TIOCPKT 0x5420
#define TIOCNOTTY 0x5422

#define TIOCPKT_DATA 0x00
#define TIOCPKT_FLUSHREAD 0x01
#define TIOCPKT_FLUSHWRITE 0x02
#define TIOCPKT_STOP 0x04
#define TIOCPKT_START 0x08
#define TIOCPKT_NOSTOP 0x10
#define TIOCPKT_DOSTOP 0x20
/* KD (keyboard/display) ioctls */
#define KDGETLED 0x4B31 /* return current led state */
#define KDSETLED 0x4B32 /* set led state [lights, not flags] */
#define KDGKBTYPE 0x4B33 /* get keyboard type */
#define KDADDIO 0x4B34 /* add i/o port as valid */
#define KDDELIO 0x4B35 /* del i/o port as valid */
#define KDENABIO 0x4B36 /* enable i/o to video board */
#define KDDISABIO 0x4B37 /* disable i/o to video board */
#define KDGKBMODE 0x4B44 /* get keyboard mode */
#define KDSKBMODE 0x4B45 /* set keyboard mode */
#define KDGKBENT 0x4B46 /* get one entry in keysym translation table */
#define KDSKBENT 0x4B47 /* set one entry in keysym translation table */
#define KDGKBSENT 0x4B48 /* get one entry in function-key string table */
#define KDSKBSENT 0x4B49 /* set one entry in function-key string table */
#define KDGKBDIACR 0x4B4A /* get kernel diacriticals table */
#define KDSKBDIACR 0x4B4B /* set kernel diacriticals table */
#define KG_SHIFT 0
#define KG_ALTGR 1
#define KG_CTRL 2
#define KG_ALT 3
#define KG_SHIFTL 4
#define KG_SHIFTR 5
#define KG_CTRLL 6
#define KG_CTRLR 7

/* Keysym type / value encoding matching Linux keyboard.h. */
#define KT_LATIN 0 /* printable latin characters */
#define KT_FN 1 /* function / navigation aliases */
#define KT_SPEC 2 /* special: K_HOLE, K_ENTER, … */
#define KT_PAD 3 /* keypad keys */
#define KT_CUR 6 /* cursor keys */
#define KT_SHIFT 7 /* modifiers */
#define KS(t, v) ((unsigned short)(((unsigned)(t) << 8) | (unsigned char)(v)))

#define K_HOLE KS(KT_SPEC, 0) /* unmapped key */
#define K_ENTER KS(KT_SPEC, 1) /* Return */
#define K_BREAK KS(KT_SPEC, 5)
#define K_CAPS KS(KT_SPEC, 7)
#define K_NUM KS(KT_SPEC, 8)
#define K_HOLD KS(KT_SPEC, 9)

#define K_P0 KS(KT_PAD, 0)
#define K_P1 KS(KT_PAD, 1)
#define K_P2 KS(KT_PAD, 2)
#define K_P3 KS(KT_PAD, 3)
#define K_P4 KS(KT_PAD, 4)
#define K_P5 KS(KT_PAD, 5)
#define K_P6 KS(KT_PAD, 6)
#define K_P7 KS(KT_PAD, 7)
#define K_P8 KS(KT_PAD, 8)
#define K_P9 KS(KT_PAD, 9)
#define K_PPLUS KS(KT_PAD, 10)
#define K_PMINUS KS(KT_PAD, 11)
#define K_PSTAR KS(KT_PAD, 12)
#define K_PSLASH KS(KT_PAD, 13)
#define K_PENTER KS(KT_PAD, 14)
#define K_PCOMMA KS(KT_PAD, 15)
#define K_PDOT KS(KT_PAD, 16)

#define K_DOWN KS(KT_CUR, 0)
#define K_LEFT KS(KT_CUR, 1)
#define K_RIGHT KS(KT_CUR, 2)
#define K_UP KS(KT_CUR, 3)

#define K_FIND KS(KT_FN, 20)
#define K_INSERT KS(KT_FN, 21)
#define K_REMOVE KS(KT_FN, 22)
#define K_SELECT KS(KT_FN, 23)
#define K_PGUP KS(KT_FN, 24)
#define K_PGDN KS(KT_FN, 25)

#define K_SHIFT KS(KT_SHIFT, KG_SHIFT)
#define K_ALT KS(KT_SHIFT, KG_ALT)
#define K_ALTGR KS(KT_SHIFT, KG_ALTGR)
#define K_CTRL KS(KT_SHIFT, KG_CTRL)
#define K_SHIFTL KS(KT_SHIFT, KG_SHIFTL)
#define K_SHIFTR KS(KT_SHIFT, KG_SHIFTR)
#define K_CTRLL KS(KT_SHIFT, KG_CTRLL)
#define K_CTRLR KS(KT_SHIFT, KG_CTRLR)

#define K_NOSUCHMAP KS(KT_SPEC, 127) /* no such translation table */

#define NR_KEYMAPS \
	256 /* modifier layers (8-bit modifier mask, matches Linux) */
#define NR_KEYS 128 /* keycodes 0–127 */

#define MAX_NR_FUNC 256 /* number of function-key string slots */
#define MAX_FUNC_STR 32 /* max bytes per function-key string (incl. NUL) */

struct kbdiacr {
	unsigned char diacr; /* the dead key */
	unsigned char base; /* the base character */
	unsigned char result; /* the combined character */
};

#define MAX_DIACR 256

struct kbdiacrs {
	unsigned int kb_cnt; /* number of valid entries */
	struct kbdiacr kbdiacr[MAX_DIACR]; /* table */
};

struct kbsentry {
	unsigned char kb_func; /* function-key index */
	unsigned char
		kb_string[MAX_FUNC_STR]; /* NUL-terminated escape sequence */
};

struct kbentry {
	unsigned char
		kb_table; /* modifier-layer index (0 = plain, 1 = shift, …) */
	unsigned char kb_index; /* keycode */
	unsigned short kb_value; /* keysym (KS(type, value)) */
};
#define K_RAW 0x00
#define K_XLATE 0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE 0x03
#define KB_84 0x01 /* 84-key keyboard */
#define KB_101 0x02 /* 101-key PC/AT keyboard */
#define KB_OTHER 0x03
#define KDSIGACCEPT 0x4B4E /* process accepts VT-switch signal */

struct kbd_repeat {
	int delay; /* msec; <= 0 means unchanged */
	int period; /* msec; <= 0 means unchanged */
};

#define KDKBDREP 0x4B52 /* set keyboard delay/repeat rate */
#define KDGETMODE 0x4B3B /* get text/graphics mode */
#define KDSETMODE 0x4B3A /* set text/graphics mode */
#define KD_TEXT 0
#define KD_GRAPHICS 1

/* Virtual-terminal ioctls */
#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_SENDSIG 0x5604
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608

#define VT_AUTO 0x00
#define VT_PROCESS 0x01
#define VT_ACKACQ 0x02

struct vt_mode {
	char mode;
	char waitv;
	short relsig;
	short acqsig;
	short frsig;
};

struct vt_stat {
	unsigned short v_active;
	unsigned short v_signal;
	unsigned short v_state;
};

/* Font ioctls */
#define GIO_FONT 0x4B60 /* get 256-char 8x8 font (raw bitmap) */
#define PIO_FONT 0x4B61 /* set 256-char 8x8 font */
#define GIO_FONTX 0x4B6B /* get font with header */
#define PIO_FONTX 0x4B6C /* set font with header */
#define KDFONTOP 0x4B72 /* font op (preferred, Linux 2.4+) */

#define KD_FONT_OP_SET 0
#define KD_FONT_OP_GET 1
#define KD_FONT_OP_SET_DEFAULT 2
#define KD_FONT_OP_COPY 3

struct console_font_op {
	unsigned int op;
	unsigned int flags;
	unsigned int width;
	unsigned int height;
	unsigned int charcount;
	unsigned char *data; /* glyph bitmap pointer */
};

struct consolefontdesc {
	unsigned short charcount;
	unsigned short charheight;
	char *chardata;
};

/* Unicode map ioctls */
#define GIO_UNIMAP 0x4B66
#define PIO_UNIMAP 0x4B67
#define PIO_UNIMAPCLR 0x4B68

struct unipair {
	unsigned short unicode;
	unsigned short fontpos;
};

struct unimapdesc {
	unsigned short entry_ct;
	struct unipair *entries;
};

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

#define NCC 8
struct termio {
	unsigned short c_iflag; /* input mode flags */
	unsigned short c_oflag; /* output mode flags */
	unsigned short c_cflag; /* control mode flags */
	unsigned short c_lflag; /* local mode flags */
	unsigned char c_line; /* line discipline */
	unsigned char c_cc[NCC]; /* control characters */
};

#define NCCS 17
struct termios {
	tcflag_t c_iflag; /* input mode flags */
	tcflag_t c_oflag; /* output mode flags */
	tcflag_t c_cflag; /* control mode flags */
	tcflag_t c_lflag; /* local mode flags */
	cc_t c_line; /* line discipline */
	cc_t c_cc[NCCS]; /* control characters */
};

/* c_cc characters */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

/* c_iflag bits */
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IUCLC 0001000
#define IXON 0002000
#define IXANY 0004000
#define IXOFF 0010000
#define IMAXBEL 0020000

/* c_oflag bits */
#define OPOST 0000001
#define OLCUC 0000002
#define ONLCR 0000004
#define OCRNL 0000010
#define ONOCR 0000020
#define ONLRET 0000040
#define OFILL 0000100
#define OFDEL 0000200
#define NLDLY 0000400
#define NL0 0000000
#define NL1 0000400
#define CRDLY 0003000
#define CR0 0000000
#define CR1 0001000
#define CR2 0002000
#define CR3 0003000
#define TABDLY 0014000
#define TAB0 0000000
#define TAB1 0004000
#define TAB2 0010000
#define TAB3 0014000
#define XTABS 0014000
#define BSDLY 0020000
#define BS0 0000000
#define BS1 0020000
#define VTDLY 0040000
#define VT0 0000000
#define VT1 0040000
#define FFDLY 0040000
#define FF0 0000000
#define FF1 0040000

/* c_cflag bit meaning */
#define CBAUD 0000017
#define B0 0000000 /* hang up */
#define B50 0000001
#define B75 0000002
#define B110 0000003
#define B134 0000004
#define B150 0000005
#define B200 0000006
#define B300 0000007
#define B600 0000010
#define B1200 0000011
#define B1800 0000012
#define B2400 0000013
#define B4800 0000014
#define B9600 0000015
#define B19200 0000016
#define B38400 0000017
#define EXTA B19200
#define EXTB B38400
#define CSIZE 0000060
#define CS5 0000000
#define CS6 0000020
#define CS7 0000040
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL 0002000
#define CLOCAL 0004000
#define CIBAUD 03600000 /* input baud rate (not used) */
#define CRTSCTS 020000000000 /* flow control */

/* c_lflag bits */
#define ISIG 0000001
#define ICANON 0000002
#define XCASE 0000004
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define ECHOCTL 0001000
#define ECHOPRT 0002000
#define ECHOKE 0004000
#define FLUSHO 0010000
#define PENDIN 0040000
#define IEXTEN 0100000

/* modem lines */
#define TIOCM_LE 0x001
#define TIOCM_DTR 0x002
#define TIOCM_RTS 0x004
#define TIOCM_ST 0x008
#define TIOCM_SR 0x010
#define TIOCM_CTS 0x020
#define TIOCM_CAR 0x040
#define TIOCM_RNG 0x080
#define TIOCM_DSR 0x100
#define TIOCM_CD TIOCM_CAR
#define TIOCM_RI TIOCM_RNG

/* tcflow() and TCXONC use these */
#define TCOOFF 0
#define TCOON 1
#define TCIOFF 2
#define TCION 3

/* tcflush() and TCFLSH use these */
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

/* tcsetattr uses these */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/*  intr=^C	quit=^|		erase=del	kill=^U
    eof=^D	vtime=\0	vmin=\1		sxtc=\0
    start=^Q	stop=^S		susp=^Z		eol=\0
    reprint=^R	discard=^U	werase=^W	lnext=^V
    eol2=\0
*/
#define INIT_C_CC "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0"

#endif // _IOCTL_H
