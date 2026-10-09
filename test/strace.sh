#!/bin/sh
# Validate syscall tracing across startup, exec, and process exit.
set -eu
probe_dir=$(mktemp -d /tmp/mos-strace.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
for command in true echo; do
    if [ "$command" = true ]; then
        strace -o "$probe_dir/trace" /bin/true > "$probe_dir/out" 2> "$probe_dir/err"
        [ ! -s "$probe_dir/out" ]
    else
        strace -o "$probe_dir/trace" /bin/echo trace-ok > "$probe_dir/out" 2> "$probe_dir/err"
        printf 'trace-ok\n' > "$probe_dir/expected"
        cmp "$probe_dir/out" "$probe_dir/expected"
        grep -E 'write\(1, "trace-ok\\n", 9\)[[:space:]]+= 9' "$probe_dir/trace"
    fi
    ! grep Stray "$probe_dir/err"
    grep -E 'execve\(.*\)[[:space:]]+= 0' "$probe_dir/trace"
    grep -E '(exit(_group)?\(0\)|\+\+\+ exited with 0 \+\+\+)'  "$probe_dir/trace"
    grep -E '(openat|mmap2|brk)\(' "$probe_dir/trace"
done
