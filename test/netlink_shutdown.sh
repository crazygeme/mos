#!/bin/sh
# Validate loopback address flushing and administrative interface transitions.
set -eu
IP=${IP:-ip}
[ "$(id -u)" -eq 0 ] || { echo 'Root privileges are required.' >&2; exit 1; }
"$IP" addr show dev lo | grep -q 'inet 127.0.0.1/8'
"$IP" link show dev lo | grep -q '<[^>]*UP'
restore_loopback() {
    "$IP" addr flush dev lo
    "$IP" addr add 127.0.0.1/8 dev lo
    "$IP" link set dev lo up
}
trap restore_loopback 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
"$IP" addr flush dev lo
if "$IP" addr show dev lo | grep -q 'inet '; then
    echo 'The loopback IPv4 address remains after flushing.' >&2
    exit 1
fi
"$IP" addr add 127.0.0.1/8 dev lo
"$IP" addr show dev lo | grep -q 'inet 127.0.0.1/8'
"$IP" link set dev lo down
if "$IP" link show dev lo | grep -q '<[^>]*UP'; then
    echo 'The loopback interface remains administratively up.' >&2
    exit 1
fi
"$IP" link set dev lo up
"$IP" link show dev lo | grep -q '<[^>]*UP'
restore_loopback
trap - 0
printf '%s\n' 'Loopback shutdown interface checks: PASS'
