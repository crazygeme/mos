#!/bin/sh

set -e
BASE=/root/tests/posix_exec_signal
mkdir -p "$BASE"
trap 'rm -rf "$BASE"' EXIT
cat > "$BASE/probe.c" <<'SOURCE'
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    sigset_t blocked, pending;
    if (argc == 1) {
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &blocked, NULL) ||
            kill(getpid(), SIGUSR1)) {
            perror("queue blocked signal");
            return 1;
        }
        execl(argv[0], argv[0], "check", (char *)NULL);
        perror("exec");
        return 1;
    }
    if (sigprocmask(SIG_BLOCK, NULL, &blocked) || sigpending(&pending)) {
        perror("read signal state");
        return 1;
    }
    if (sigismember(&blocked, SIGUSR1) != 1 ||
        sigismember(&pending, SIGUSR1) != 1) {
        fprintf(stderr, "Blocked pending SIGUSR1 must survive exec\n");
        return 1;
    }
    return 0;
}
SOURCE
gcc -O2 -Wall "$BASE/probe.c" -o "$BASE/probe"
"$BASE/probe"
