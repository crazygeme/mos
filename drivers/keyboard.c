#include <drivers/keyboard.h>
#include <lib/klib.h>
#include <int/int.h>
#include <drivers/keymap.h>
#include <int/dsr.h>
#include <ps/lock.h>
#include <config.h>

static struct _key_buf
{
  unsigned len;
  unsigned write_idx;
  unsigned read_idx;
  semaphore lock;
  spinlock idx_lock;
  unsigned char buf[KEYBOARD_BUF_LEN];
}key_buf;

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
    unsigned char first_scancode;     /* First scancode. */
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

static int map_key (const struct keymap[], unsigned scancode, unsigned char *);

static void kb_dsr(void* param);


// this will wait if empty
unsigned char kb_buf_get()
{
  unsigned length = 0;
  length = key_buf.len;
  unsigned read_idx;
  unsigned char ret;

  if (length == 0)
    {
      sema_wait(&key_buf.lock);
    }

  spinlock_lock(&key_buf.idx_lock);

  read_idx = key_buf.read_idx;
  ret = key_buf.buf[read_idx];
  read_idx++;
  if (read_idx == KEYBOARD_BUF_LEN)
    read_idx = 0;

  key_buf.read_idx = read_idx;
  key_buf.len--;
  if (key_buf.len == 0)
    sema_reset(&key_buf.lock);

  spinlock_unlock(&key_buf.idx_lock);

  return ret;
}

// this will drop key if full
static void kb_buf_put(unsigned char key)
{
    unsigned length = 0;
    unsigned write_idx;
    int needs_trigger = 0;

    length = key_buf.len;

    if (length == KEYBOARD_BUF_LEN)
      return;

    if (length == 0)
      needs_trigger = 1;

    spinlock_lock(&key_buf.idx_lock);

    write_idx = key_buf.write_idx;
    key_buf.buf[write_idx] = key;
    write_idx++;
    if (write_idx == KEYBOARD_BUF_LEN)
      write_idx = 0;
    key_buf.write_idx = write_idx;
    key_buf.len++;

    spinlock_unlock(&key_buf.idx_lock);

    if (needs_trigger)
      sema_trigger(&key_buf.lock);
}

void kb_init()
{
  int i = 0;

  for (i = 0; i < KEYBOARD_BUF_LEN; i++)
    {
      key_buf.buf[i] = 0;
    }
  key_buf.len = key_buf.read_idx = key_buf.write_idx = 0;
  sema_init(&key_buf.lock, "keyboard", 1);
  spinlock_init(&key_buf.idx_lock);
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

  /* Read scancode, including second byte if prefix code. */
  code = _read_port (KB_DATA);
  if (code == 0xe0)
    code = (code << 8) | _read_port (KB_DATA);

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
  else if (map_key (invariant_keymap, code, &c)
           || (!shift && map_key (unshifted_keymap, code, &c))
           || (shift && map_key (shifted_keymap, code, &c)))
    {
      /* Ordinary character. */
      if (!release) 
        {
          /* Reboot if Ctrl+Alt+Del pressed. */
          if (c == 'C' && ctrl && alt)
            reboot ();

          if (c == 'P'  && ctrl && alt)
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
          // FIXME
          kb_buf_put(c);
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
map_key (const struct keymap k[], unsigned scancode, unsigned char *c) 
{
  for (; k->first_scancode != 0; k++)
    if (scancode >= k->first_scancode
        && scancode < k->first_scancode + strlen (k->chars)) 
      {
        *c = k->chars[scancode - k->first_scancode];
        return 1; 
      }

  return 0;
}

