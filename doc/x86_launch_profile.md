# x86 Process Launch Profile

Date: 2026-10-06. Revision: `2e6426d` (`refine multi-arch codes`).

The release x86 kernel completed the 1,000-process benchmark in a median of
0.13 seconds with one virtual CPU and 0.12 seconds with two. PIT clock access
accounted for approximately 10% of active samples in both configurations.
Explicit TLB-flush routines were rarely sampled. These measurements identify
clock access as an optimization candidate; they do not establish the benefit
of PCID on x64.

## Configuration and workload

The kernel was built with `make -j8 ARCH=x86 BUILD=release`. The host processor
was an AMD Ryzen 9 7950X. QEMU used KVM, the `coreduo` CPU model, 4 GiB RAM,
VMware VGA, and one or two virtual CPUs. The RH9 disk was opened with
`snapshot=on`; temporary disk changes were discarded when each VM exited.

The guest reported Bash 2.05b.0, GNU coreutils 4.5.3, and glibc 2.3.2. The
userspace source archives were obtained before analyzing program behavior.
The benchmark scripts were unchanged:

```sh
# test.sh
for i in $(seq 1000); do /bin/true; done

# full.sh
for i in $(seq 10); do echo $i; time -p ./test.sh; done
```

Each timed run included the shell executing `test.sh`, its `seq` invocation,
and 1,000 executions of `/bin/true`. Timings were captured from an ordinary
`full.sh` execution without monitor sampling. Output was redirected in the
guest and read after all ten runs completed.

The kernel was booted directly using `-kernel out/x86/release/kernel -append
'bash verbose=0'`. This starts the benchmark without the complete RH9 init
and desktop workload used by the GRUB boot path. Networking used QEMU user
networking. Consequently, these results are not a controlled reproduction of
the earlier Linux comparison. No fresh Linux measurement or pre-optimization
MOS comparison was performed.

## Elapsed times

| Virtual CPUs | Ten reported real times, seconds | Median | Mean | Range |
| --- | --- | --- | --- | --- |
| 1 | .14, .14, .14, .14, .13, .12, .12, .12, .12, .13 | .130 | .130 | .12–.14 |
| 2 | .14, .14, .14, .13, .12, .12, .12, .12, .12, .12 | .120 | .127 | .12–.14 |

Both sequences show a warm-up trend. The 0.01-second reporting precision and
overlapping ranges prevent attributing a small difference to CPU count.
The reported user and system times were not used to estimate kernel costs.

Raw captures: [one CPU](../out/profiling-x86/timing-1cpu-full.txt),
[two CPUs](../out/profiling-x86/timing-2cpu-full.txt), and
[guest versions](../out/profiling-x86/guest-versions.txt).

## Sampling results

Separate profiling runs repeatedly executed `test.sh`. A QMP monitor driver
paused the VM, read each CPU's registers using an explicit `cpu-index`, and
resumed execution after each sample. The running interval was randomized
between 1 and 5 milliseconds. Symbols came from the matching release
`kernel.dbg`, including assembly symbols without size metadata.

Each main capture lasted 40 seconds of host wall time, including monitor
overhead. The one-CPU capture contained 4,429 active samples. The two-CPU
capture contained 5,812 samples, of which 2,906 were in `smp_idle`. The table
excludes idle samples; userspace remains part of the denominator.

| Sample location | One CPU, % active | Two CPUs, % active |
| --- | ---: | ---: |
| Userspace | 16.26 | 17.00 |
| Page-fault entry (`intr0e_stub`) | 9.19 | 8.12 |
| Port reads and writes from `time_now_us` | 9.80 | 10.50 |
| `_spinlock_lock.part.0` | 5.80 | 6.13 |
| `load_ldt` | 3.61 | 3.58 |
| `mm_destroy_user_map` | 3.25 | 3.65 |
| `mm_copy_phys_page` | 2.48 | 2.34 |
| `tlb_flush` | 0 samples | 0.07 |

These are sampled instruction locations, not hardware cycle measurements.
Privileged instructions and emulated device access may stop at recognizable
instruction boundaries. The sampling pauses also perturb execution. The
figures therefore provide prioritization evidence rather than exact savings
predictions or a complete attribution of KVM host work.

Raw captures: [one CPU](../out/profiling-x86/release-1cpu-profile.json) and
[two CPUs](../out/profiling-x86/release-2cpu-profile.json).

## Clock callers and other costs

All port samples in the main captures returned into `time_now_us`. The ports
were PIT data at `0x40` and the latch command at `0x43`.
[The PIT clock implementation](../src/driver/impl/timer/pit.c) latches and reads
the hardware counter on each clock query.

A supplementary 20-second two-CPU capture collected 1,394 active samples,
including 165 PIT port samples. Caller addresses were read using stack offsets
verified against the exact release disassembly, rather than a general frame
pointer walk. Clock origins accounted for those 165 samples as follows:

| Origin | PIT samples |
| --- | ---: |
| `ps_fire_timers_unsafe` | 68 |
| `ext4_touch_file` | 63 |
| `setup_stack` | 29 |
| `timer_arm_unsafe` | 3 |
| Network service update | 1 |
| System service task | 1 |

[Scheduler timer checking](../src/ps/impl/alg/ps_alg_rr.c) reads the clock before
examining timer deadlines. [Filesystem timestamp updates](../src/fs/impl/root.c)
read the clock before updating inode timestamps. [ELF stack setup](../arch/abi/elf_stack.h)
reads the clock to seed the random generator. Reducing PIT queries in these
paths, or supplying a faster clock source, is a concrete next investigation.
Timer accuracy and filesystem timestamp semantics must remain correct.

[LDT loading](../arch/x86/ps/impl/task.c) performs `LLDT 0` even when a task has
no LDT. Nearly every `load_ldt` sample was at the return immediately after that
instruction. Tracking the loaded LDT per CPU is another candidate, subject to
correct task and descriptor lifecycle handling.

The sampled [spinlock acquisition](../src/lib/impl/lock.c) costs appear on one
CPU as well as two. Samples concentrated around atomic exchanges, including
the exchange used to save interrupt state; none appeared in the retry loop.
This evidence indicates acquisition overhead rather than measured contention.
Page-fault entry and memory copying also warrant investigation. A page fault
is not an ordinary TLB miss.

Supplementary capture:
[clock callers](../out/profiling-x86/release-2cpu-clock-callers.json).

## Implications for PCID

PCID is not available in this 32-bit execution mode. A low sample count inside
`tlb_flush` does not measure translation refill costs after CR3 changes;
those costs occur at subsequent memory accesses. Hardware TLB-miss counters
could not be collected because privileged `perf` access required interactive
authentication on the host.

An x64 comparison with PCID enabled and disabled, together with translation
miss counters, is needed to quantify its benefit. This x86 profile supports
investigating PIT clock queries first, without ruling out a PCID improvement.
No kernel source was changed during profiling, and all profiling VMs were
stopped after collection.

The measurement driver is preserved at
[`out/profiling-x86/profile_driver.py`](../out/profiling-x86/profile_driver.py).
Its memory addresses, structure offsets, and caller stack offsets are specific
to this build and must be revalidated before use with another revision.
