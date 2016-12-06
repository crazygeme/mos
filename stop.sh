#!/bin/bash
kill -SIGKILL $(ps -C qemu-system-i386 -o pid --no-headers)
