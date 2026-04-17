# Testing

MOS has two test facilities that work together:

1. **Kernel-mode tests** (`test/*.c`) — C functions compiled into the kernel, exercising kernel internals directly via the `KTEST` framework.
2. **User-mode shell script tests** (`test/*.sh`) — POSIX shell scripts that run inside the booted VM, testing syscall behaviour and user-space interfaces.

Both are opt-in: they are only compiled and linked when building `kernel-test` (`make test`).

---

## Build and Run

```sh
make test          # build out/x86/release/kernel-test
make test-debug    # build out/x86/debug/kernel-test
make run-test      # build and boot the release test kernel
make run-test-debug # build and boot the debug test kernel
./run.sh test      # same as run-test
./run.sh test debug # same as run-test-debug
```

The kernel command line receives `"test"`, which sets `TestControl.test = 1`.
In test mode:
- `init` launches `test.sh` (the test harness entry point) instead of a login shell.
- Networking is disabled (replaced by user-mode NAT for isolation).
- When `init` exits, `system_down()` is called and QEMU exits via the debug exit port.  `run.sh` maps the exit code back to the test result.

---

## 1. Kernel-Mode Tests

### Framework

**Header:** `include/test/test.h`  
**Runner:** `src/proc/common/test.c`  
**Source files:** `test/*.c`

Tests are written in C and linked directly into the kernel image. They run in kernel mode — ring 0, with full access to kernel memory and APIs.

### Defining a Test

```c
#include <test/test.h>

KTEST(SuiteName, TestName)
{
    void *p = malloc(64);
    ASSERT_NONNULL(p);
    free(p);
    return 0;    // 0 = pass
}
```

`KTEST(Suite, Name)` expands to:
1. A `static int _ktest_fn_Suite_Name(void)` function definition.
2. A `ktest_t` record placed in the `.ktest` ELF section.

The linker collects all `.ktest` entries between `__ktest_start` and `__ktest_end`. **No manual registration call is needed.**

The function must return `int`: `0` = pass, non-zero (typically `__LINE__`) = fail. `ASSERT_*` macros return `__LINE__` automatically via early return; always add `return 0;` at the end.

### Assertions

**Fatal — abort the current test on failure:**

| Macro                | Fails when      |
| -------------------- | --------------- |
| `ASSERT_TRUE(cond)`  | `cond` is false |
| `ASSERT_FALSE(cond)` | `cond` is true  |
| `ASSERT_EQ(a, b)`    | `a != b`        |
| `ASSERT_NE(a, b)`    | `a == b`        |
| `ASSERT_NULL(p)`     | `p != NULL`     |
| `ASSERT_NONNULL(p)`  | `p == NULL`     |
| `ASSERT_GE(a, b)`    | `a < b`         |
| `ASSERT_GT(a, b)`    | `a <= b`        |
| `ASSERT_LE(a, b)`    | `a > b`         |
| `ASSERT_LT(a, b)`    | `a >= b`        |

**Non-fatal — record failure and continue:**

Same names with `EXPECT_` prefix (e.g. `EXPECT_EQ`). They set `_ktest_expect_failed` but do not return early.

### Running Tests

Tests are run by writing a filter to `/proc/tests/.runner`:

```sh
echo all          > /proc/tests/.runner   # run all kernel tests
echo 1            > /proc/tests/.runner   # same as "all"
echo MallocTest   > /proc/tests/.runner   # exact suite name
echo Malloc*      > /proc/tests/.runner   # glob pattern
```

Each suite also has a convenience script:

```sh
sh /proc/tests/MallocTest    # runs that suite via .runner
sh /proc/tests/all           # runs all kernel tests
```

Output format mirrors Google Test:

```
[==========] Running 7 tests from 1 test suite.
[----------] 7 tests from malloc
[ RUN      ] malloc.zero
[       OK ] malloc.zero (0 ms)
[ RUN      ] malloc.pattern
[       OK ] malloc.pattern (1 ms)
[  FAILED  ] malloc.bad_case (3 ms)
[==========] 7 tests ran.
[  PASSED  ] 6 tests.
[  FAILED  ] 1 test.
[  FAILED  ] malloc.bad_case
```

`klog` output goes to the serial port (captured by QEMU's `-serial stdio`).

### What You CAN Do

- Call any exported kernel function: `malloc`/`free`/`zalloc`, `vm_alloc`/`vm_free`, `do_mmap`/`do_munmap`, locking primitives, etc.
- Read and write kernel global variables (`phymm_used`, `heap_quota`, etc.) to detect leaks.
- Dereference kernel virtual addresses (≥ `KERNEL_OFFSET = 0xC0000000`).
- Access `CURRENT_TASK()` (the test runner task).
- Exercise the uncontended fast path of locks (single-task, no blocking needed).

### What You CANNOT Do

- **No large stack arrays.** The kernel stack is < 4 KB. Use `malloc`/`free` for any buffer > ~64 bytes. - **No user-space pointers.** Tests run in kernel mode; user VA is not mapped.
- **No blocking that requires a second task.** Contended locks, `time_wait`, or anything that yields to the scheduler and requires another task to make progress will deadlock (the test suite runs in a single-task context).
- **No libc.** Use kernel equivalents: `klog` (not `printf`), `kmalloc`/`kfree`, `memcpy`, `strlen`, etc.
- **No testing the syscall layer directly** from a `KTEST`. Kernel tests call internal C functions, not `int $0x80`.
- **No signal delivery** (signal delivery requires returning to user mode via `iret`).

### Adding a New Kernel Test

1. Create `test/yourmodule_test.c` (or add to an existing file in `test/`).
2. Include `<test/test.h>` and any relevant kernel headers.
3. Write `KTEST(SuiteName, TestName) { ... return 0; }` blocks.
4. Run `make test` or `make test-debug` — the new file is automatically picked up by the Makefile (`TEST_SRCS = $(shell find test/ -name '*.c')`).

No Makefile edits needed.

### Example: Leak-Checking Pattern

```c
KTEST(mm, vm_alloc_no_leak)
{
    unsigned phys_before = phymm_used;

    unsigned addr = vm_alloc(1);
    ASSERT_NE(addr, 0u);
    EXPECT_EQ(phymm_used, phys_before + 1);

    vm_free(addr, 1);
    EXPECT_EQ(phymm_used, phys_before);   // no physical page leak
    return 0;
}
```

---

## 2. User-Mode Shell Script Tests

### Framework

**Source files:** `test/*.sh`  
**Code generator:** `tools/gen_ktest_scripts.sh`  
**Runner integration:** `src/proc/common/test.c`

Each `.sh` file is a POSIX shell script that runs inside the booted OS under a normal user-space `sh` process. The Makefile converts every `.sh` into a C source file (using `tools/gen_ktest_scripts.sh`) and links it into the kernel via the `KTEST_SCRIPT_NAMED` macro. The script body is embedded as a string literal in the `.ktest_script` ELF section.

At runtime, opening `/proc/tests/<scriptname>` synthesizes a wrapper shell script that:
1. Records the start time from `/proc/uptime`.
2. Runs the script body in a subshell, capturing stdout+stderr to a temp log.
3. Records the end time.
4. Writes `[ RUN ]` / `[  OK  ]` / `[ FAILED ]` lines (with elapsed ms) to `/proc/tests/.result`, which echoes them via `klog`.
5. On failure, dumps the captured log through `.result`.
6. Removes the temp log and exits with the script's exit code.

### Running Scripts

```sh
sh /proc/tests/posix_io       # run one script
sh /proc/tests/all_script     # run all shell-script tests in sequence
```

`all_script` exits with the total failure count (0 = all passed).

### Writing a Script

The script must exit 0 on success and non-zero on failure. `set -e` is the standard way to make any failed command abort the script.

Typical pattern:

```sh
#!/bin/sh

set -e
BASE=/root/my_test   # work directory on the OS disk

fail()
{
    echo "my_test: $1" >&2
    exit 1
}

expect_eq()
{
    [ "$1" = "$2" ] && return 0
    fail "Expected '$1', got '$2' ($3)"
}

expect_failure()
{
    set +e
    output=$("$@" 2>&1)
    rc=$?
    set -e
    [ "$rc" -ne 0 ] && return 0
    fail "Expected failure from '$*', got success"
}

cleanup() { rm -rf "$BASE" 2>/dev/null || true; }
trap cleanup EXIT
cleanup

mkdir -p "$BASE"
printf 'hello\n' > "$BASE/file.txt"
data=$(cat "$BASE/file.txt")
expect_eq "hello" "$data" "file contents"

expect_failure cat "$BASE/nonexistent"
```

### What Scripts CAN Do

- Any shell command available on the OS: `cat`, `echo`, `printf`, `mkdir`, `rm`, `ls`, `wc`, `head`, `tr`, `grep`, etc.
- Pipelines and subshells.
- Testing syscall-level behaviour by observing exit codes and output of standard commands.
- Testing file-system semantics (create/read/write/delete/rename/symlinks/hard links).
- Testing process semantics (fork/exec via subshells, pipes, signals via `kill`).
- Writing to `/proc/tests/.result` to log additional information.

### What Scripts CANNOT Do

- **No kernel-internal state.** Scripts cannot check `phymm_used` or `heap_quota`.
- **No timing guarantees** beyond what `/proc/uptime` offers; do not rely on wall-clock sub-millisecond precision.
- **No networking** in test mode (network is disabled when `TestControl.test = 1`).
- **No programs not present on the disk image** (glibc RH9 userspace: `sh`, `bash`, `cat`, `ls`, `grep`, `awk`, `sed`, standard POSIX tools, gcc, vim, emacs are present; anything else may not be).

### Adding a New Shell Script Test

1. Create `test/myfeature.sh` with `#!/bin/sh` at the top.
2. Make it exit 0 on success, non-zero on failure.
3. Run `make test` or `make test-debug` — the Makefile finds it via `$(shell find test/ -name '*.sh' | sort)`, generates `out/x86/<build>/generated/test/myfeature.c`, compiles and links it.

No Makefile edits needed.

---

## 3. Inline Shell Scripts from C (`KTEST_SCRIPT`)

For short scripts that naturally belong next to C code, `KTEST_SCRIPT` embeds the script inline:

```c
#include <test/test.h>

KTEST_SCRIPT(my_inline_test,
    "#!/bin/sh\n"
    "set -e\n"
    "echo hello | grep -q hello\n"
);
```

`KTEST_SCRIPT_NAMED("name", content)` is the lower-level form when the name must be a string literal.

These appear in `/proc/tests/` alongside `.sh`-derived scripts and are included in `all_script`.

---

## 4. `/proc/tests/` Directory Reference

| Entry           | Type       | Description                                               |
| --------------- | ---------- | --------------------------------------------------------- |
| `.runner`       | write-only | Write a filter (`all`, suite name, or glob) to run ktests |
| `.result`       | write-only | Echo a line to `klog` (used by script wrappers)           |
| `.tty_state`    | read-only  | TTY metadata snapshot (used by tty tests)                 |
| `all`           | executable | Script: `echo all > /proc/tests/.runner`                  |
| `all_script`    | executable | Script: run all `KTEST_SCRIPT` entries in sequence        |
| `<SuiteName>`   | executable | Script: `echo <SuiteName> > /proc/tests/.runner`          |
| `<script_name>` | executable | Wrapped shell script from `test/<script_name>.sh`         |

`/proc/tests/` is only mounted when at least one `KTEST` or `KTEST_SCRIPT` entry exists in the image.
