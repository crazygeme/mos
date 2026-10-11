#!/usr/bin/env python3
"""Validate PS/2 LED acknowledgments, retries, and scancode preservation."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
src = (root / 'src/driver/input/ps2_keyboard.c').read_text()
code = src[src.index('static int kb_send_command_byte('):src.index('static void kb_worker(void *param)\n{')]
preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#define PS2_PORT_KEYBOARD 0
static unsigned char replies[256], writes[32], input[256];
static unsigned nr, ri, nw, ni, begins, ends;
static int write_error, read_error;
static int ps2_write_byte(unsigned port, unsigned char b)
{ assert(port == 0); writes[nw++] = b; return write_error; }
static int ps2_read_reply(unsigned port, unsigned char *b, int spins)
{ assert(port == 0 && spins > 0); if (read_error < 0) return read_error;
  if (ri == nr) return -ETIMEDOUT;
  *b = replies[ri++]; return 0; }
static void kb_receive_byte(unsigned char b) { input[ni++] = b; }
static void ps2_command_begin_preserve(unsigned port)
{ assert(port == 0); begins++; }
static void ps2_command_end(unsigned port) { assert(port == 0); ends++; }
static void reset(void)
{ nr=ri=nw=ni=begins=ends=0; write_error=read_error=0; }
'''
tests = r'''
int main(void) {
 reset(); replies[nr++]=0xfa; replies[nr++]=0xfa;
 assert(kb_set_leds(4)==0 && nw==2 && writes[0]==0xed && writes[1]==4);
 assert(begins==1 && ends==1 && ni==0);
 reset(); replies[nr++]=0x1e; replies[nr++]=0x9e;
 replies[nr++]=0xfa; replies[nr++]=0xfa;
 assert(kb_set_leds(6)==0 && ni==2 && input[0]==0x1e && input[1]==0x9e);
 reset(); replies[nr++]=0xfe; replies[nr++]=0xfa; replies[nr++]=0xfa;
 assert(kb_set_leds(0)==0 && nw==3 && writes[0]==0xed && writes[1]==0xed && writes[2]==0);
 reset(); replies[nr++]=0xfe; replies[nr++]=0xfe; replies[nr++]=0xfe;
 assert(kb_set_leds(4)==-EIO && nw==3 && begins==ends);
 reset(); write_error=-ETIMEDOUT;
 assert(kb_set_leds(4)==-ETIMEDOUT && begins==ends);
 reset(); replies[nr++]=0xfa;
 assert(kb_set_leds(4)==-ETIMEDOUT && begins==ends);
 reset(); assert(kb_set_leds(8)==-EINVAL && begins==0 && nw==0);
 puts("PS/2 keyboard LED checks passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='mos-keyboard-leds-') as tmp:
 p=Path(tmp); (p/'test.c').write_text(preamble+code+tests)
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
