#include <keyboard.h>
#include <klib.h>
#include <int.h>
#include <keymap.h>
#include <dsr.h>
#include <lock.h>
#include <config.h>
#include <cyclebuf.h>
#include <profiling.h>

static cy_buf* buf;


/* Current state of shift keys.
   True if depressed, 0 otherwise. */
static int left_shift, right_shift;    /* Left and right Shift keys. */
static int left_alt, right_alt;        /* Left and right Alt keys. */
static int left_ctrl, right_ctrl;      /* Left and right Ctl keys. */

/* Status of Caps Lock.
   True when on, 0 when off. */
static int caps_lock;

/* Maps a set of contiguous scancodes into characters. */
struct keymap
{
    unsigned short first_scancode;     /* First scancode. */
    const char *chars;          /* chars[0] has scancode first_scancode,
                                   chars[1] has scancode first_scancode + 1,
                                   and so on to the end of the string. */
};

/* Keys that produce the same characters regardless of whether
   the Shift keys are down.  Case of letters is an exception
   that we handle elsewhere.  */
static const struct keymap invariant_keymap[] =
{
  {0x01, "\033"},             /* Escape. */
  {0x0e, "\b"},
  {0x0f, "\tQWERTYUIOP"},
  {0x1c, "\r"},
  {0x1e, "ASDFGHJKL"},
  {0x2c, "ZXCVBNM"},
  {0x37, "*"},
  {0x39, " "},
  {0x53, "\177"},             /* Delete. */
  {0, NULL},
};

/* Characters for keys pressed without Shift, for those keys
   where it matters. */
static const struct keymap unshifted_keymap[] =
{
  {0x02, "1234567890-="},
  {0x1a, "[]"},
  {0x27, ";'`"},
  {0x2b, "\\"},
  {0x33, ",./"},
  {0, NULL},
};

/* Characters for keys pressed with Shift, for those keys where
   it matters. */
static const struct keymap shifted_keymap[] =
{
  {0x02, "!@#$%^&*()_+"},
  {0x1a, "{}"},
  {0x27, ":\"~"},
  {0x2b, "|"},
  {0x33, "<>?"},
  {0, NULL},
};

static const struct keymap special_keymap[] =
{
  {0xe04b, "\033[D"},           /* Left */
  {0xe04d, "\033[C"},           /* Right */
  {0xe048, "\033[A"},           /* Up */
  {0xe050, "\033[B"},           /* Down */
  {0xe049, "\033[5"},           /* Page Up */
  {0xe051, "\033[6"},           /* Page Down */
  {0xe047, "\033[1"},           /* Home */
  {0xe04f, "\033[4"},           /* End */
  {0xe052, "\033[2"},           /* Insert */
  //{0xe053, "\b"},           /* Delete */
  {0, NULL},
};


static int map_key(const struct keymap[], unsigned scancode, unsigned char *);

static void kb_dsr(void* param);

static const char* map_special_key(const struct keymap[], unsigned scancode);

// this will wait if empty
unsigned char kb_buf_get()
{

    return cyb_getc(buf);
}

int kb_can_read()
{
    return !cyb_isempty(buf);
}

// this will drop key if full
static void kb_buf_put(unsigned char key)
{
    return cyb_putc(buf, key);
}

void kb_init()
{
    buf = cyb_create("keyboard");
    int_register(0x21, kb_process, 0, 0);
}

void kb_process(intr_frame *frame)
{
    dsr_add(kb_dsr, 0);
}


static void kb_dsr(void* param)
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

    const char* special = 0;
    /* Read scancode, including second byte if prefix code. */
    code = _read_port(KB_DATA);
    if (code == 0xe0)
        code = (code << 8) | _read_port(KB_DATA);

    /* Bit 0x80 distinguishes key press from key release
       (even if there's a prefix). */
    release = (code & 0x80) != 0;
    code &= ~0x80u;

    /* Interpret key. */
    if (code == 0x3a)
    {
        /* Caps Lock. */
        if (!release)
            caps_lock = !caps_lock;
    }
    else if (map_key(invariant_keymap, code, &c)
        || (!shift && map_key(unshifted_keymap, code, &c))
        || (shift && map_key(shifted_keymap, code, &c)))
    {
        /* Ordinary character. */
        if (!release)
        {
            /* Reboot if Ctrl+Alt+C pressed. */
            if (c == 'C' && ctrl && alt)
                reboot();

            if (c == 'V'  && ctrl && alt)
                shutdown();

            /* Handle Ctrl, Shift.
               Note that Ctrl overrides Shift. */
            if (ctrl && c >= 0x40 && c < 0x60)
            {
                /* A is 0x41, Ctrl+A is 0x01, etc. */
                c -= 0x40;
            }
            else if (shift == caps_lock)
                c = tolower(c);

            /* Handle Alt by setting the high bit.
               This 0x80 is unrelated to the one used to
               distinguish key press from key release. */
            if (alt)
                c += 0x80;

            /* Append to keyboard buffer. */
            kb_buf_put(c);
        }
    }
    else if (special = map_special_key(special_keymap, code))
    {
        if (!release)
        {
            while (*special)
            {
                kb_buf_put(*special++);
            }
        }
    }
    else
    {
        /* Maps a keycode into a shift state variable. */
        struct shift_key
        {
            unsigned scancode;
            int *state_var;
        };

        /* Table of shift keys. */
        static const struct shift_key shift_keys[] =
        {
          {  0x2a, &left_shift},
          {  0x36, &right_shift},
          {  0x38, &left_alt},
          {0xe038, &right_alt},
          {  0x1d, &left_ctrl},
          {0xe01d, &right_ctrl},
          {0,      NULL},
        };

        const struct shift_key *key;

        /* Scan the table. */
        for (key = shift_keys; key->scancode != 0; key++)
            if (key->scancode == code)
            {
                *key->state_var = !release;
                break;
            }
    }
}

/* Scans the array of keymaps K for SCANCODE.
   If found, sets *C to the corresponding character and returns
   1.
   If not found, returns 0 and C is ignored. */
static int
map_key(const struct keymap k[], unsigned scancode, unsigned char *c)
{
    for (; k->first_scancode != 0; k++)
        if (scancode >= k->first_scancode
            && scancode < k->first_scancode + strlen(k->chars))
        {
            *c = k->chars[scancode - k->first_scancode];
            return 1;
        }

    return 0;
}


static const char* map_special_key(const struct keymap k[], unsigned scancode)
{
    for (; k->first_scancode != 0; k++)
    {
        if (k->first_scancode == scancode)
        {
            return k->chars;
        }
    }

    return 0;
}
