#include "errno.h"
#include <ps/ps.h>
#include <int/int.h>
#include <int/dsr.h>
#include <lib/port.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <hw/keyboard.h>
#include <hw/keymap.h>
#include <hw/tty.h>
#include <fs/ioctl.h>
#include <config.h>

#define I8042_STATUS 0x64
#define I8042_STATUS_OBF 0x01
#define I8042_STATUS_AUX 0x20

/* ── Keysym translation table (KDGKBENT / KDSKBENT) ─────────────────────── */

static unsigned short key_maps[NR_KEYMAPS][NR_KEYS];

/* ── Function-key string table (KDGKBSENT / KDSKBSENT) ──────────────────── */

static char func_strings[MAX_NR_FUNC][MAX_FUNC_STR];

/* Default Linux-console function-key escape sequences */
static const char *const func_defaults[] = {
	/* F1–F5: linux-console style \033[[A..E */
	"\033[[A",
	"\033[[B",
	"\033[[C",
	"\033[[D",
	"\033[[E",
	/* F6–F12: VT220 style */
	"\033[17~",
	"\033[18~",
	"\033[19~",
	"\033[20~",
	"\033[21~",
	"\033[23~",
	"\033[24~",
};

static void kbd_init_func_strings(void)
{
	unsigned int i;
	for (i = 0; i < MAX_NR_FUNC; i++)
		func_strings[i][0] = '\0';
	for (i = 0; i < sizeof(func_defaults) / sizeof(func_defaults[0]); i++) {
		strncpy(func_strings[i], func_defaults[i], MAX_FUNC_STR - 1);
		func_strings[i][MAX_FUNC_STR - 1] = '\0';
	}
}

int kbd_get_kbsentry(struct kbsentry *kbs)
{
	strncpy((char *)kbs->kb_string, func_strings[kbs->kb_func],
		sizeof(kbs->kb_string) - 1);
	kbs->kb_string[sizeof(kbs->kb_string) - 1] = '\0';
	return 0;
}

int kbd_set_kbsentry(const struct kbsentry *kbs)
{
	strncpy(func_strings[kbs->kb_func], (const char *)kbs->kb_string,
		MAX_FUNC_STR - 1);
	func_strings[kbs->kb_func][MAX_FUNC_STR - 1] = '\0';
	return 0;
}

/* Convenience shorthand used only during table init */
#define KSL(c) KS(KT_LATIN, (c))
#define KSF(n) KS(KT_FN, (n))

static void kbd_init_keymaps(void)
{
	int t, k;
	unsigned short *p, *s;

	/* Default all entries to K_HOLE */
	for (t = 0; t < NR_KEYMAPS; t++)
		for (k = 0; k < NR_KEYS; k++)
			key_maps[t][k] = K_HOLE;

	/* ── Table 0: plain ──────────────────────────────────────────────── */
	p = key_maps[0];
	p[1] = KSL(27); /* ESC */
	p[2] = KSL('1');
	p[3] = KSL('2');
	p[4] = KSL('3');
	p[5] = KSL('4');
	p[6] = KSL('5');
	p[7] = KSL('6');
	p[8] = KSL('7');
	p[9] = KSL('8');
	p[10] = KSL('9');
	p[11] = KSL('0');
	p[12] = KSL('-');
	p[13] = KSL('=');
	p[14] = KSL(127); /* Backspace → DEL */
	p[15] = KSL('\t'); /* Tab */
	p[16] = KSL('q');
	p[17] = KSL('w');
	p[18] = KSL('e');
	p[19] = KSL('r');
	p[20] = KSL('t');
	p[21] = KSL('y');
	p[22] = KSL('u');
	p[23] = KSL('i');
	p[24] = KSL('o');
	p[25] = KSL('p');
	p[26] = KSL('[');
	p[27] = KSL(']');
	p[28] = K_ENTER; /* Return */
	p[29] = K_CTRL;
	p[30] = KSL('a');
	p[31] = KSL('s');
	p[32] = KSL('d');
	p[33] = KSL('f');
	p[34] = KSL('g');
	p[35] = KSL('h');
	p[36] = KSL('j');
	p[37] = KSL('k');
	p[38] = KSL('l');
	p[39] = KSL(';');
	p[40] = KSL('\'');
	p[41] = KSL('`');
	p[42] = K_SHIFTL;
	p[43] = KSL('\\');
	p[44] = KSL('z');
	p[45] = KSL('x');
	p[46] = KSL('c');
	p[47] = KSL('v');
	p[48] = KSL('b');
	p[49] = KSL('n');
	p[50] = KSL('m');
	p[51] = KSL(',');
	p[52] = KSL('.');
	p[53] = KSL('/');
	p[54] = K_SHIFTR;
	p[55] = K_PSTAR;
	p[56] = K_ALT;
	p[57] = KSL(' '); /* Space */
	p[58] = K_CAPS;
	for (k = 0; k < 10; k++)
		p[59 + k] = KSF(k); /* F1–F10 */
	p[69] = K_NUM;
	p[70] = K_HOLD;
	p[71] = K_P7;
	p[72] = K_P8;
	p[73] = K_P9;
	p[74] = K_PMINUS;
	p[75] = K_P4;
	p[76] = K_P5;
	p[77] = K_P6;
	p[78] = K_PPLUS;
	p[79] = K_P1;
	p[80] = K_P2;
	p[81] = K_P3;
	p[82] = K_P0;
	p[83] = K_PDOT;
	p[87] = KSF(10); /* F11 */
	p[88] = KSF(11); /* F12 */
	p[96] = K_PENTER;
	p[97] = K_CTRLR;
	p[98] = K_PSLASH;
	p[100] = K_ALTGR;
	p[102] = K_FIND;
	p[103] = K_UP;
	p[104] = K_PGUP;
	p[105] = K_LEFT;
	p[106] = K_RIGHT;
	p[107] = K_SELECT;
	p[108] = K_DOWN;
	p[109] = K_PGDN;
	p[110] = K_INSERT;
	p[111] = K_REMOVE;

	/* ── Table 1: shift ──────────────────────────────────────────────── */
	s = key_maps[1];
	s[1] = KSL(27);
	s[2] = KSL('!');
	s[3] = KSL('@');
	s[4] = KSL('#');
	s[5] = KSL('$');
	s[6] = KSL('%');
	s[7] = KSL('^');
	s[8] = KSL('&');
	s[9] = KSL('*');
	s[10] = KSL('(');
	s[11] = KSL(')');
	s[12] = KSL('_');
	s[13] = KSL('+');
	s[14] = KSL(8); /* Shift+Backspace → BS */
	s[15] = KSL('\t');
	s[16] = KSL('Q');
	s[17] = KSL('W');
	s[18] = KSL('E');
	s[19] = KSL('R');
	s[20] = KSL('T');
	s[21] = KSL('Y');
	s[22] = KSL('U');
	s[23] = KSL('I');
	s[24] = KSL('O');
	s[25] = KSL('P');
	s[26] = KSL('{');
	s[27] = KSL('}');
	s[28] = K_ENTER;
	s[29] = K_CTRL;
	s[30] = KSL('A');
	s[31] = KSL('S');
	s[32] = KSL('D');
	s[33] = KSL('F');
	s[34] = KSL('G');
	s[35] = KSL('H');
	s[36] = KSL('J');
	s[37] = KSL('K');
	s[38] = KSL('L');
	s[39] = KSL(':');
	s[40] = KSL('"');
	s[41] = KSL('~');
	s[42] = K_SHIFTL;
	s[43] = KSL('|');
	s[44] = KSL('Z');
	s[45] = KSL('X');
	s[46] = KSL('C');
	s[47] = KSL('V');
	s[48] = KSL('B');
	s[49] = KSL('N');
	s[50] = KSL('M');
	s[51] = KSL('<');
	s[52] = KSL('>');
	s[53] = KSL('?');
	s[54] = K_SHIFTR;
	s[55] = K_PSTAR;
	s[56] = K_ALT;
	s[57] = KSL(' ');
	s[58] = K_CAPS;
	for (k = 0; k < 10; k++)
		s[59 + k] = KSF(k);
	s[69] = K_NUM;
	s[70] = K_HOLD;
	s[71] = K_P7;
	s[72] = K_P8;
	s[73] = K_P9;
	s[74] = K_PMINUS;
	s[75] = K_P4;
	s[76] = K_P5;
	s[77] = K_P6;
	s[78] = K_PPLUS;
	s[79] = K_P1;
	s[80] = K_P2;
	s[81] = K_P3;
	s[82] = K_P0;
	s[83] = K_PDOT;
	s[87] = KSF(10);
	s[88] = KSF(11);
	s[96] = K_PENTER;
	s[97] = K_CTRLR;
	s[98] = K_PSLASH;
	s[100] = K_ALTGR;
	s[102] = K_FIND;
	s[103] = K_UP;
	s[104] = K_PGUP;
	s[105] = K_LEFT;
	s[106] = K_RIGHT;
	s[107] = K_SELECT;
	s[108] = K_DOWN;
	s[109] = K_PGDN;
	s[110] = K_INSERT;
	s[111] = K_REMOVE;
}

#undef KSL
#undef KSF

int kbd_get_kbentry(struct kbentry *kbe)
{
	if (kbe->kb_index >= NR_KEYS)
		return -EINVAL;
	kbe->kb_value = key_maps[kbe->kb_table][kbe->kb_index];
	return 0;
}

int kbd_set_kbentry(const struct kbentry *kbe)
{
	if (kbe->kb_index >= NR_KEYS)
		return -EINVAL;
	/* K_NOSUCHMAP: loadkeys requests deallocation of this table.
	 * We have a flat array so just clear the whole row to K_HOLE. */
	if (kbe->kb_value == K_NOSUCHMAP) {
		int k;
		for (k = 0; k < NR_KEYS; k++)
			key_maps[kbe->kb_table][k] = K_HOLE;
		return 0;
	}
	key_maps[kbe->kb_table][kbe->kb_index] = kbe->kb_value;
	return 0;
}

/* Current state of shift keys.
   True if depressed, 0 otherwise. */
static int left_shift, right_shift; /* Left and right Shift keys. */
static int left_alt, right_alt; /* Left and right Alt keys. */
static int left_ctrl, right_ctrl; /* Left and right Ctl keys. */

/* Status of Caps Lock.
   True when on, 0 when off. */
static int caps_lock;

/* Maps a set of contiguous scancodes into characters. */
struct keymap {
	unsigned short first_scancode; /* First scancode. */
	const char *chars; /* chars[0] has scancode first_scancode,
			      chars[1] has scancode first_scancode + 1,
			      and so on to the end of the string. */
};

/* Keys that produce the same characters regardless of whether
   the Shift keys are down.  Case of letters is an exception
   that we handle elsewhere.  */
static const struct keymap invariant_keymap[] = {
	{ 0x01, "\033" }, /* Escape. */
	{ 0x0e, "\b" },	       { 0x0f, "\tQWERTYUIOP" }, { 0x1c, "\r" },
	{ 0x1e, "ASDFGHJKL" }, { 0x2c, "ZXCVBNM" },	 { 0x37, "*" },
	{ 0x39, " " },	       { 0x53, "\177" }, /* Delete. */
	{ 0, NULL },
};

/* Characters for keys pressed without Shift, for those keys
   where it matters. */
static const struct keymap unshifted_keymap[] = {
	{ 0x02, "1234567890-=" }, { 0x1a, "[]" },  { 0x27, ";'`" },
	{ 0x2b, "\\" },		  { 0x33, ",./" }, { 0, NULL },
};

/* Characters for keys pressed with Shift, for those keys where
   it matters. */
static const struct keymap shifted_keymap[] = {
	{ 0x02, "!@#$%^&*()_+" }, { 0x1a, "{}" },
	{ 0x27, ":\"~" },	  { 0x2b, "|" },
	{ 0x33, "<>?" },	  { 0, NULL },
};

static const struct keymap special_keymap[] = {
	{ 0xe04b, "\033[D" }, /* Left */
	{ 0xe04d, "\033[C" }, /* Right */
	{ 0xe048, "\033[A" }, /* Up */
	{ 0xe050, "\033[B" }, /* Down */
	{ 0xe049, "\033[5" }, /* Page Up */
	{ 0xe051, "\033[6" }, /* Page Down */
	{ 0xe047, "\033[1" }, /* Home */
	{ 0xe04f, "\033[4" }, /* End */
	{ 0xe052, "\033[2" }, /* Insert */
	//{0xe053, "\b"},           /* Delete */
	{ 0, NULL },
};

static int map_key(const struct keymap[], unsigned scancode, unsigned char *);

static void kb_dsr(void *param);

static const char *map_special_key(const struct keymap[], unsigned scancode);
static int mediumraw_keycode(unsigned scancode);
static void update_shift_state(unsigned scancode, int release);
static void kb_handle_scancode(unsigned code, int kb_mode);

static void emit_kb_bytes(const unsigned char *bytes, unsigned len)
{
	unsigned i;

	for (i = 0; i < len; i++)
		tty_active_kb_put(bytes[i]);
}

static void emit_raw_scancode(unsigned raw_code)
{
	unsigned char bytes[2];
	unsigned len = 0;

	if (raw_code > 0xff)
		bytes[len++] = (unsigned char)(raw_code >> 8);
	bytes[len++] = (unsigned char)raw_code;
	emit_kb_bytes(bytes, len);
}

static void emit_mediumraw(unsigned scancode, int release)
{
	int keycode = mediumraw_keycode(scancode);
	unsigned char code;

	if (keycode <= 0)
		return;

	code = (unsigned char)keycode;
	if (release)
		code |= 0x80;
	tty_active_kb_put(code);
}

static void update_shift_state(unsigned scancode, int release)
{
	struct shift_key {
		unsigned scancode;
		int *state_var;
	};

	static const struct shift_key shift_keys[] = {
		{ 0x2a, &left_shift }, { 0x36, &right_shift },
		{ 0x38, &left_alt },   { 0xe038, &right_alt },
		{ 0x1d, &left_ctrl },  { 0xe01d, &right_ctrl },
		{ 0, NULL },
	};

	const struct shift_key *key;

	for (key = shift_keys; key->scancode != 0; key++) {
		if (key->scancode == scancode) {
			*key->state_var = !release;
			break;
		}
	}
}

void kb_init()
{
	kbd_init_keymaps();
	kbd_init_func_strings();
	int_register(0x21, kb_process, 0, 0);
}

static int kb_dsr_armed = 0;
static unsigned kb_prefix;

void kb_process(intr_frame *frame)
{
	if (!kb_dsr_armed) {
		kb_dsr_armed = 1;
		if (!dsr_add(kb_dsr, 0)) {
			/* DSR dropped: drain the data port now so the PS/2
			 * controller can generate the next interrupt, then
			 * clear the armed flag so the next keypress retries. */
			port_read_byte(KB_DATA);
			kb_dsr_armed = 0;
		}
	}
}

static void kb_dsr(void *param)
{
	unsigned char st;
	int kb_mode = tty_active_kb_mode();
	kb_dsr_armed = 0;

	while ((st = port_read_byte(I8042_STATUS)) & I8042_STATUS_OBF) {
		unsigned code;

		if (st & I8042_STATUS_AUX)
			break;

		code = port_read_byte(KB_DATA);
		if (code == 0xe0 || code == 0xe1) {
			kb_prefix = code;
			continue;
		}
		if (kb_prefix) {
			code |= kb_prefix << 8;
			kb_prefix = 0;
		}

		kb_handle_scancode(code, kb_mode);
	}
}

static void kb_handle_scancode(unsigned raw_code, int kb_mode)
{
	/* Status of shift keys. */
	int shift = left_shift || right_shift;
	int alt = left_alt || right_alt;
	int ctrl = left_ctrl || right_ctrl;

	/* Keyboard scancode. */
	unsigned code = raw_code;

	/* False if key pressed, 1 if key released. */
	int release;

	/* Character that corresponds to `code'. */
	unsigned char c;

	const char *special = 0;

	/* Bit 0x80 distinguishes key press from key release
	 * (even if there's a prefix). */
	release = (code & 0x80) != 0;
	code &= ~0x80u;
	update_shift_state(code, release);

	/* Keep kernel VT hotkeys working even when userspace requested
	 * raw keyboard bytes on the active console.
	 * Ctrl+1..0: switch virtual terminal (scancodes 2..11 → tty 1..10) */
	if (!release && ctrl && code >= 2 && code <= 11) {
		tty_switch((int)(code - 1));
		return;
	}

	if (kb_mode == K_RAW) {
		emit_raw_scancode(raw_code);
		return;
	}
	if (kb_mode == K_MEDIUMRAW) {
		emit_mediumraw(code, release);
		return;
	}

	if (!release && ctrl && code >= 2 && code <= 11) {
		tty_switch((int)(code - 1));
		return;
	}

	/* Interpret key. */
	if (code == 0x3a) {
		/* Caps Lock. */
		if (!release)
			caps_lock = !caps_lock;
	} else if (map_key(invariant_keymap, code, &c) ||
		   (!shift && map_key(unshifted_keymap, code, &c)) ||
		   (shift && map_key(shifted_keymap, code, &c))) {
		/* Ordinary character. */
		if (!release) {
			/* Reboot if Ctrl+Alt+C pressed. */
			if (c == 'C' && ctrl && alt)
				reboot();

			if (c == 'V' && ctrl && alt)
				shutdown();

			/* Handle Ctrl, Shift.
               		 * Note that Ctrl overrides Shift. */
			if (ctrl && c >= 0x40 && c < 0x60) {
				/* A is 0x41, Ctrl+A is 0x01, etc. */
				c -= 0x40;
			} else if (shift == caps_lock)
				c = tolower(c);

			/* Handle Alt by setting the high bit.
               		 * This 0x80 is unrelated to the one used to
              		 * distinguish key press from key release. */
			if (alt)
				c += 0x80;

			/* Route to active TTY's keyboard buffer. */
			tty_active_kb_put(c);
		}
	} else if ((special = map_special_key(special_keymap, code))) {
		if (!release) {
			while (*special) {
				tty_active_kb_put((unsigned char)*special++);
			}
		}
	} else {
		/* Modifier state was already updated before the mode dispatch. */
	}
}

/* Scans the array of keymaps K for SCANCODE.
   If found, sets *C to the corresponding character and returns 1.
   If not found, returns 0 and C is ignored. */
static int map_key(const struct keymap k[], unsigned scancode, unsigned char *c)
{
	for (; k->first_scancode != 0; k++)
		if (scancode >= k->first_scancode &&
		    scancode < k->first_scancode + strlen(k->chars)) {
			*c = k->chars[scancode - k->first_scancode];
			return 1;
		}

	return 0;
}

static const char *map_special_key(const struct keymap k[], unsigned scancode)
{
	for (; k->first_scancode != 0; k++) {
		if (k->first_scancode == scancode) {
			return k->chars;
		}
	}

	return 0;
}

static int mediumraw_keycode(unsigned scancode)
{
	switch (scancode) {
	case 0xe01c:
		return 96;
	case 0xe01d:
		return 97;
	case 0xe035:
		return 98;
	case 0xe037:
		return 99;
	case 0xe038:
		return 100;
	case 0xe047:
		return 102;
	case 0xe048:
		return 103;
	case 0xe049:
		return 104;
	case 0xe04b:
		return 105;
	case 0xe04d:
		return 106;
	case 0xe04f:
		return 107;
	case 0xe050:
		return 108;
	case 0xe051:
		return 109;
	case 0xe052:
		return 110;
	case 0xe053:
		return 111;
	default:
		break;
	}

	if (scancode <= 0x7f)
		return (int)scancode;
	return -1;
}
