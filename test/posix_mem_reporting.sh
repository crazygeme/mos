#!/bin/sh
# Physical-memory totals must agree across both proc interfaces.
set -eu

awk -F '|' '
/^LowTotal:/ {
    split($0, fields, /[ \t]+/)
    low_bytes = fields[2] * 1024
    low_seen = 1
}
/^HighTotal:/ {
    split($0, fields, /[ \t]+/)
    high_bytes = fields[2] * 1024
    high_seen = 1
}
NF == 6 {
    name = $2
    gsub(/^[ \t]+|[ \t]+$/, "", name)
    if (name == "low phys total") {
        mos_low_bytes = $4 + 0
        mos_low_seen = 1
    } else if (name == "high phys total") {
        mos_high_bytes = $4 + 0
        mos_high_seen = 1
    }
}
END {
    if (!low_seen || !high_seen || !mos_low_seen || !mos_high_seen) {
        print "Physical-memory total fields are missing."
        exit 1
    }
    if (mos_low_bytes != low_bytes || mos_high_bytes != high_bytes) {
        printf "Memory totals differ: meminfo low=%.0f high=%.0f; mos low=%.0f high=%.0f\n", low_bytes, high_bytes, mos_low_bytes, mos_high_bytes
        exit 1
    }
    print "Physical-memory totals agree across proc interfaces."
}
' /proc/meminfo /proc/mos
