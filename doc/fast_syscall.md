# Fast system calls

MOS supports these entry paths:

| Kernel | User ABI | Entry | Return |
| --- | --- | --- | --- |
| x86 | i386 through `AT_SYSINFO` | `SYSENTER` when CPUID.1:EDX.SEP is set | `SYSEXIT` for matching frames; otherwise `IRET` |
| x86 | i386 on CPUs without SEP, or direct legacy calls | `INT 0x80` | `IRET` |
| x64 | AMD64 | `SYSCALL` | `SYSRETQ` for matching native frames; otherwise `IRETQ` |
| x64 | i386 compatibility mode | `INT 0x80` | `IRETQ` |

Both fast entries construct the existing `intr_frame` and use the common
syscall dispatcher, tracing, signal delivery, TLS refresh, and return flag
sanitization. Syscall numbers and argument registers do not change.

## i386 SYSENTER

Each CPU detects SEP and initializes `IA32_SYSENTER_CS/EIP/ESP`. Dedicated
adjacent ring-0 code/data descriptors occupy GDT slots 11 and 12. Slots 13 and
14 provide flat ring-3 code/data aliases for SYSEXIT; the original user
selectors remain valid for ELF entry and legacy returns. Task activation updates SYSENTER_ESP to
the current task's kernel stack along with TSS.ESP0.

`mm_vdso_fastcall_entry()` supplies a SYSENTER wrapper as `AT_SYSINFO` on
supported CPUs, or the original INT wrapper otherwise. The wrapper saves
ECX, EDX and EBP on the user stack, then points EBP at that save area before
SYSENTER. This follows the [Linux i386 vDSO convention](https://github.com/torvalds/linux/blob/master/arch/x86/entry/vdso/vdso32/system_call.S).

The entry builds a complete privilege frame with the vDSO landing pad as
its return IP. It saves the incoming arithmetic/DF flags, restores user IF,
and clears live TF/DF/NT/AC before calling kernel C. After kernel segments
are installed and interrupts enabled, the saved EBP is read as the sixth
argument. A bad stack terminates the calling process with SIGSEGV.

SYSEXIT requires the original vDSO landing address and save-area stack,
the expected alias CS/SS, unchanged ECX/EDX, a user-space stack address,
IF set, and TF/RF/VM clear. Signals and modified debugger frames retain IRET.
The fast return restores arithmetic, DF, and IOPL flags with IF masked, then
uses `STI; SYSEXIT` so the interrupt shadow covers the stack and privilege
handoff. The wrapper restores EBP/EDX/ECX and returns to its caller.

Syscall entry retains the standard flat user DS/ES descriptors when present;
other selectors are replaced before kernel C runs. Live GS always uses a
kernel descriptor because nested interrupt frames can resume under another
task's GDT. Every exit restores the saved selectors. TLS refresh checks for
unchanged installed descriptors before masking interrupts; updates still use
the masked path.

Direct INT 0x80 calls, including existing signal restorers, remain supported.
The normal syscall assembly now jumps directly to `ret_from_syscall` after
its dispatcher returns; only fork children run `ret_from_fork`'s separate
signal-delivery step.

## AMD64 SYSCALL/SYSRETQ

SYSCALL swaps in the CPU GS base, saves user RSP in CPU-local storage, and
switches to the current task's kernel stack. A temporary entry marker lets
the common register-save code retain the kernel GS base without an extra
SWAPGS pair and RDMSR. The marker is cleared before entering the dispatcher.
Ordinary interrupts keep their existing GS-base detection, including NMI
handling during partial syscall entry/return.

SYSRET derives user CS and SS from STAR rather than from the saved frame.
The x64 GDT now uses compatibility CS=0x1b, shared user SS=0x23, and native
CS=0x2b; STAR is programmed for this order on every CPU. Kernel selectors
and the native code selector are unchanged. See the [Intel instruction reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf)
for the selector and canonical-address requirements.

The return assembly uses SYSRETQ only if:

- The frame is a syscall with the expected native CS and SS.
- Saved RCX equals return RIP, and saved R11 equals return RFLAGS.
- RIP and RSP are both below 2^47, in the canonical user half.
- TF and RF are clear.

All other frames use IRETQ, preserving register edits made by signal
handlers, sigreturn, or debugging. Interrupts remain masked during register
restoration, GS handoff, and the final return. Compile-time assertions guard
the assembly's frame offsets and selector relationships.

## Validation

Host adapter unit tests exercise production wire adapters with isolated kernel
services. The tests require a C compiler and compile their probes when invoked.

```sh
python3 -m unittest discover -s test -p 'test_abi_adapters.py'
```

`tools/user/fast_syscall_probe.c` contains the freestanding entry-path probe.
It covers AT_SYSINFO selection, ENOSYS, DF restoration, mmap arguments,
signal return, scheduling, ptrace stops, and legacy INT 0x80 entry.
It also verifies carry-flag preservation. Run isolated KVM regressions with:

```sh
python3 tools/tests/vfs_fastpath_qemu.py --probe syscall --smp 4 --output out/syscall-check.log
python3 tools/tests/vfs_fastpath_qemu.py --probe syscall --cpu coreduo,-sep --output out/int80-check.log
```
