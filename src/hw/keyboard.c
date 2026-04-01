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
	p[55] = KSL('*'); /* Keypad * */
	p[57] = KSL(' '); /* Space */
	for (k = 0; k < 10; k++)
		p[59 + k] = KSF(k); /* F1–F10 */
	p[87] = KSF(10); /* F11 */
	p[88] = KSF(11); /* F12 */

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
	s[55] = KSL('*');
	s[57] = KSL(' ');
	for (k = 0; k < 10; k++)
		s[59 + k] = KSF(k);
	s[87] = KSF(10);
	s[88] = KSF(11);
}

#undef KSL
#undef KSF

int kbd_get_kbentry(struct kbentry *kbe)
{
	if (kbe->kb_table >= NR_KEYMAPS || kbe->kb_index >= NR_KEYS)
		return -EINVAL;
	kbe->kb_value = key_maps[kbe->kb_table][kbe->kb_index];
	return 0;
}

int kbd_set_kbentry(const struct kbentry *kbe)
{
	if (kbe->kb_table >= NR_KEYMAPS || kbe->kb_index >= NR_KEYS)
		return -EINVAL;
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

void kb_init()
{
	kbd_init_keymaps();
	kbd_init_func_strings();
	int_register(0x21, kb_process, 0, 0);
}

static int kb_dsr_armed = 0;
void kb_process(intr_frame *frame)
{
	if (!kb_dsr_armed) {
		kb_dsr_armed = 1;
		dsr_add(kb_dsr, 0);
	}
}

static void kb_dsr(void *param)
{
	/* Status of shift keys. */
	int shift = left_shift || right_shift;
	int alt = left_alt || right_alt;
	int ctrl = left_ctrl || right_ctrl;

	/* Keyboard scancode. */
	unsigned code;

	/* False if key pressed, 1 if key released. */
	int release;

	/* Character that corresponds to `code'. */
	unsigned char c;

	const char *special = 0;

	kb_dsr_armed = 0;

	/* Read scancode, including second byte if prefix code. */
	code = port_read_byte(KB_DATA);
	if (code == 0xe0)
		code = (code << 8) | port_read_byte(KB_DATA);

	/* Bit 0x80 distinguishes key press from key release
       (even if there's a prefix). */
	release = (code & 0x80) != 0;
	code &= ~0x80u;

	/* Ctrl+Alt+1..0: switch virtual terminal (scancodes 2..11) */
	if (!release && ctrl && code >= 2 && code <= 11) {
		tty_switch((int)(code - 2));
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
		/* Maps a keycode into a shift state variable. */
		struct shift_key {
			unsigned scancode;
			int *state_var;
		};

		/* Table of shift keys. */
		static const struct shift_key shift_keys[] = {
			{ 0x2a, &left_shift }, { 0x36, &right_shift },
			{ 0x38, &left_alt },   { 0xe038, &right_alt },
			{ 0x1d, &left_ctrl },  { 0xe01d, &right_ctrl },
			{ 0, NULL },
		};

		const struct shift_key *key;

		/* Scan the table. */
		for (key = shift_keys; key->scancode != 0; key++)
			if (key->scancode == code) {
				*key->state_var = !release;
				break;
			}
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
