#!/usr/bin/env python3
"""Exercise production wire adapters with isolated host-side kernel services."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHIMS = {
    'ps/ps.h': r'''
#ifndef TEST_PS_H
#define TEST_PS_H
#include <stdint.h>
#include <stddef.h>
#include <ps/task.h>
typedef struct _arch_intr_frame intr_frame;
typedef struct {
    ptrace_saved_frame ptrace_frame;
    int ptrace_frame_valid;
    uintptr_t ptrace_orig_eax, ptrace_eventmsg;
    unsigned uid, euid, gid, egid;
} test_user;
typedef struct _task_struct {
    void *robust_list_head;
    size_t robust_list_size;
    int (*robust_list_reader)(struct _task_struct *, uintptr_t,
                             uintptr_t *, intptr_t *, uintptr_t *);
    test_user *user;
    struct { unsigned short cs; } tss;
    int status;
} task_struct;
extern task_struct *current;
enum { ps_stopped = 1 };
#define SIGKILL 9
void do_exit(int);
task_struct *ps_find_process(int);
int ps_read_process_memory(task_struct *, const void *, void *, unsigned);
#endif
''',
    'lib/klib.h': '#include <string.h>\n#include <stdlib.h>\n#define kmalloc malloc\n#define kfree free\n',
    'int/int.h': '#include <arch/config.h>\n',
    'mm/mm.h': '#include <arch/config.h>\n#define PAGE_SIZE MOS_PAGE_SIZE\n#define USER_HEAP_END 0x40000000U\n',
    'mm/vdso.h': '#include <stdint.h>\nuintptr_t mm_vdso_fastcall_entry(void);\nvoid mm_vdso_map(void);\n',
    'device/time.h': '#include <stdint.h>\nuint64_t time_now_us(void);\n',
    'syscall/syscall.h': r'''
#include <stdint.h>
int sys_shmget(int, unsigned, int);
intptr_t sys_shmat(int, const void *, int);
int sys_shmdt(const void *);
''',
    'macro.h': '#define TEST_LOG(x) 0\n#define TEST_LOG_INFO 0\n#define klog(...) ((void)0)\n',
}
PROBE = r'''
#include <assert.h>
#include <arch/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ps/ptrace.h>
#include <syscall/ipc.h>
#include <elf/format.h>
#include <arch/abi/i386/services.h>
int native_set_robust_list(void *, size_t);
int native_get_robust_list(int, void *, void *);
int native_ptrace(int, int, void *, void *);
int native_shmctl(int, int, void *);
static test_user user;
static task_struct task = { .user = &user, .status = ps_stopped };
task_struct *current = &task;
task_struct *ps_find_process(int pid) { return pid == 7 ? &task : NULL; }
int ps_read_process_memory(task_struct *target, const void *src, void *dst, unsigned n)
{ assert(target == &task); memcpy(dst, src, n); return 0; }
int ps_ptrace_target(int pid, task_struct **out)
{ *out = ps_find_process(pid); return *out ? 0 : -ESRCH; }
int ps_ptrace_control(int request, int pid, void *addr, void *data)
{ (void)pid; (void)addr; (void)data; return request == PTRACE_CONT ? 123 : -EINVAL; }
int sys_shmget(int key, unsigned size, int flags)
{ (void)key; (void)size; (void)flags; return 7; }
intptr_t sys_shmat(int id, const void *addr, int flags)
{ (void)id; (void)addr; (void)flags; return -EINVAL; }
int sys_shmdt(const void *addr) { (void)addr; return 0; }
int ps_shmctl(int id, int command, struct shm_status *out)
{
    if (id != 7) return -EINVAL;
    if ((command & ~0x100) == 0) return 0;
    if ((command & ~0x100) != 2) return -ENOSYS;
    if (!out) return -EFAULT;
    *out = (struct shm_status){ .key = 99, .uid = 12, .gid = 13,
        .mode = 0600, .ctime = 1234, .cpid = 7, .nattch = 2, .size = 8192 };
    return 0;
}
struct _file { const unsigned char *bytes; size_t length; };
int elf_read(file *fp, unsigned offset, void *out, int length)
{
    if (offset > fp->length || (unsigned)length > fp->length - offset) return 0;
    memcpy(out, fp->bytes + offset, length); return length;
}
void do_exit(int sig) { (void)sig; abort(); }
uint64_t time_now_us(void) { return 1234; }
uintptr_t mm_vdso_fastcall_entry(void) { return 0xbffff100U; }
static int vdso_maps;
void mm_vdso_map(void) { vdso_maps++; }
static void canary(const unsigned char *buf, size_t first, size_t end)
{ for (size_t i = first; i < end; i++) assert(buf[i] == 0xa5); }
int main(void)
{
    struct { uint32_t next; int32_t offset; uint32_t pending; } h32 =
        { 0xf0000010, -8, 0x80000010 };
    struct { uint64_t next; int64_t offset; uint64_t pending; } h64 =
        { 0x100000010ULL, -16, 0x200000010ULL };
    uintptr_t next, pending;
    intptr_t offset;
    assert(i386_set_robust_list(&h32, 24) == -EINVAL);
    assert(i386_set_robust_list(&h32, 12) == 0);
    assert(task.robust_list_reader(&task, (uintptr_t)&h32, &next, &offset, &pending) == 0);
    assert(next == h32.next && offset == -8 && pending == h32.pending);
    assert(task.robust_list_reader(&task, (uintptr_t)&h32, &next, NULL, NULL) == 0);
    assert(next == h32.next);
    uint64_t head = 0, length = 0;
    assert(native_get_robust_list(7, &head, &length) == 0);
    assert(head == (uintptr_t)&h32 && length == 12);
    assert(native_set_robust_list(&h64, 24) == 0);
    assert(task.robust_list_reader(&task, (uintptr_t)&h64, &next, &offset, &pending) == 0);
    assert(next == h64.next && offset == -16 && pending == h64.pending);
    uint32_t compat_head[2] = { 0, 0xa5a5a5a5 }, compat_len[2] = { 0, 0xa5a5a5a5 };
    assert(i386_get_robust_list(7, compat_head, compat_len) == 0);
    assert(compat_head[0] == (uint32_t)(uintptr_t)&h64 && compat_len[0] == 24);
    assert(compat_head[1] == 0xa5a5a5a5 && compat_len[1] == 0xa5a5a5a5);
    assert(native_get_robust_list(8, &head, &length) == -ESRCH);

    user.ptrace_frame_valid = 1;
    user.ptrace_frame.ebx = 0x123456789abcdef0ULL;
    user.ptrace_frame.eip = 0x100000010ULL;
    user.ptrace_frame.cs = USER64_CODE_SELECTOR;
    user.ptrace_orig_eax = 59;
    _Alignas(uint64_t) unsigned char output[256];
    memset(output, 0xa5, sizeof(output));
    assert(i386_ptrace(PTRACE_GETREGS, 7, NULL, output) == 0);
    uint32_t *regs32 = (void *)output;
    assert(regs32[0] == 0x9abcdef0 && regs32[11] == 59 && regs32[12] == 0x10);
    canary(output, 68, sizeof(output));
    memset(output, 0xa5, sizeof(output));
    assert(native_ptrace(PTRACE_GETREGS, 7, NULL, output) == 0);
    uint64_t *regs64 = (void *)output;
    assert(regs64[5] == user.ptrace_frame.ebx && regs64[15] == 59);
    assert(regs64[16] == user.ptrace_frame.eip && regs64[17] == USER64_CODE_SELECTOR);
    canary(output, 216, sizeof(output));
    uint64_t memory = 0x1122334455667788ULL;
    memset(output, 0xa5, sizeof(output));
    assert(i386_ptrace(PTRACE_PEEKDATA, 7, &memory, output) == 0);
    assert(*(uint32_t *)output == 0x55667788); canary(output, 4, sizeof(output));
    memset(output, 0xa5, sizeof(output));
    assert(native_ptrace(PTRACE_PEEKDATA, 7, &memory, output) == 0);
    assert(*(uint64_t *)output == memory); canary(output, 8, sizeof(output));
    assert(i386_ptrace(PTRACE_PEEKUSER, 7, (void *)1, output) == -EIO);
    assert(native_ptrace(PTRACE_PEEKUSER, 7, (void *)4, output) == -EIO);
    assert(native_ptrace(PTRACE_CONT, 7, NULL, NULL) == 123);
    user.ptrace_frame_valid = 0;
    assert(i386_ptrace(PTRACE_GETREGS, 7, NULL, output) == -EIO);

    memset(output, 0xa5, sizeof(output));
    assert(i386_shmctl(7, 2, output) == 0);
    assert(*(uint32_t *)(output + 24) == 8192);
    assert(*(uint32_t *)(output + 36) == 1234);
    canary(output, 56, sizeof(output));
    memset(output, 0xa5, sizeof(output));
    assert(native_shmctl(7, 0x102, output) == 0);
    assert(*(uint64_t *)(output + 48) == 8192);
    assert(*(uint64_t *)(output + 72) == 1234);
    canary(output, 112, sizeof(output));
    assert(i386_shmctl(7, 2, NULL) == -EFAULT);
    assert(native_shmctl(7, 0, NULL) == 0);
    assert(native_shmctl(7, 3, output) == -ENOSYS);

    Elf32_Ehdr h32_elf = { .e_machine = EM_386, .e_ehsize = sizeof(Elf32_Ehdr),
        .e_phentsize = sizeof(Elf32_Phdr), .e_entry = 0x8048000, .e_phnum = 1 };
    Elf64_Ehdr h64_elf = { .e_machine = EM_X86_64, .e_ehsize = sizeof(Elf64_Ehdr),
        .e_phentsize = sizeof(Elf64_Phdr), .e_entry = 0x100400000ULL, .e_phnum = 1 };
    file f32 = { (void *)&h32_elf, sizeof(h32_elf) };
    file f64 = { (void *)&h64_elf, sizeof(h64_elf) };
    Elf64_Ehdr decoded;
    assert(elf_i386_format.read_header(&f32, &decoded) == 0);
    assert(decoded.e_entry == h32_elf.e_entry && decoded.e_phnum == 1);
    assert(elf_amd64_format.read_header(&f64, &decoded) == 0);
    assert(decoded.e_entry == h64_elf.e_entry);
    h32_elf.e_machine = EM_X86_64;
    assert(elf_i386_format.read_header(&f32, &decoded) == -ENOEXEC);
    h64_elf.e_phentsize = sizeof(Elf32_Phdr);
    assert(elf_amd64_format.read_header(&f64, &decoded) == -ENOEXEC);

    _Alignas(uint64_t) unsigned char stack[4096];
    char *argv[] = { "program", "arg" }, *envp[] = { "KEY=value" };
    mos_binfmt fmt = { .elf_load_addr = 0x8048034, .e_phent = 32,
        .e_phnum = 1, .e_entry = 0x8048000 };
    uintptr_t top = (uintptr_t)stack + sizeof(stack);
    uintptr_t sp = elf_i386_format.setup_stack("/bin/program", 2, argv, 1, envp, top, &fmt);
    assert(!(sp & 15) && sp >= (uintptr_t)stack);
    uint32_t *words32 = (void *)sp;
    assert(words32[0] == 2 && words32[3] == 0 && words32[5] == 0);
    assert(words32[6] == 3 && words32[7] == fmt.elf_load_addr);
    assert(words32[8] == 4 && words32[9] == 32);
    assert(words32[34] == 32 && words32[35] == 0xbffff100U);
    fmt.e_phent = 56; fmt.e_entry = 0x100400000ULL;
    sp = elf_amd64_format.setup_stack("/bin/program", 2, argv, 1, envp, top, &fmt);
    assert(!(sp & 15) && sp >= (uintptr_t)stack);
    uint64_t *words64 = (void *)sp;
    assert(words64[0] == 2 && words64[3] == 0 && words64[5] == 0);
    assert(!strcmp((char *)(uintptr_t)words64[1], "program"));
    assert(!strcmp((char *)(uintptr_t)words64[2], "arg"));
    assert(!strcmp((char *)(uintptr_t)words64[4], "KEY=value"));
    assert(words64[8] == 4 && words64[9] == 56);
    assert(words64[18] == 9 && words64[19] == fmt.e_entry);
    assert(words64[34] == 1 && words64[35] == 0);
    elf_i386_format.activate(&task);
    assert(task.tss.cs == USER_CODE_SELECTOR && task.robust_list_size == 12 && vdso_maps == 1);
    elf_amd64_format.activate(&task);
    assert(task.tss.cs == USER64_CODE_SELECTOR && task.robust_list_size == 24 && vdso_maps == 1);
    puts("ABI adapter checks passed: robust readers/getters, ptrace, SHM, ELF headers and initial stacks.");
    return 0;
}
'''


def main():
    sources = [
        'arch/abi/i386/robust.c', 'arch/x64/syscall/impl/robust.c',
        'arch/abi/i386/ptrace.c', 'arch/x64/syscall/impl/ptrace.c',
        'arch/abi/i386/ipc.c', 'arch/x64/syscall/impl/ipc.c',
        'arch/abi/i386/elf.c', 'arch/x64/ps/impl/elf.c',
    ]
    with tempfile.TemporaryDirectory(prefix='mos-abi-adapters-') as directory:
        temp = Path(directory)
        for name, content in SHIMS.items():
            path = temp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        probe = temp / 'probe.c'
        probe.write_text(PROBE)
        binary = temp / 'probe'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I', str(temp), '-I', str(ROOT / 'arch/x64'),
                        '-I', str(ROOT / 'src'), '-I', str(ROOT),
                        str(probe), *(str(ROOT / name) for name in sources),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
