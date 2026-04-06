#!/bin/sh

set -e

TTY=
SNAP=/tmp/tty_vc_$$.snap
META=

fail()
{
	echo "tty_vc: $1" >&2
	exit 1
}

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

cleanup()
{
	rm -f "$SNAP" >/dev/null 2>&1 || true
}

meta_get()
{
	key=$1
	for tok in $META; do
		case "$tok" in
		"$key"=*)
			printf '%s\n' "${tok#*=}"
			return 0
			;;
		esac
	done
	return 1
}

line_get()
{
	line=$1
	key=$2
	for tok in $line; do
		case "$tok" in
		"$key"=*)
			printf '%s\n' "${tok#*=}"
			return 0
			;;
		esac
	done
	return 1
}

snapshot()
{
	cat /proc/tests/.tty_state > "$SNAP" || fail "failed to read /proc/tests/.tty_state"
	META=$(sed -n '1p' "$SNAP")
	[ -n "$META" ] || fail "missing tty meta line"
}

expect_meta()
{
	key=$1
	want=$2
	got=$(meta_get "$key") || fail "missing meta key: $key"
	[ "$got" = "$want" ] && return 0
	fail "Expected: meta $key = '$want'
Actual: '$got'"
}

expect_cell()
{
	row=$1
	col=$2
	want_ch=$3
	want_fg=$4
	want_bg=$5
	line=$(sed -n "/^cell $row $col /{p;q;}" "$SNAP")
	[ -n "$line" ] || fail "missing cell line for row=$row col=$col"
	got_ch=$(line_get "$line" ch) || fail "missing ch for row=$row col=$col"
	got_fg=$(line_get "$line" fg) || fail "missing fg for row=$row col=$col"
	got_bg=$(line_get "$line" bg) || fail "missing bg for row=$row col=$col"
	[ "$got_ch" = "$want_ch" ] || fail "Expected: cell($row,$col) ch = '$want_ch'
Actual: '$got_ch'"
	[ "$got_fg" = "$want_fg" ] || fail "Expected: cell($row,$col) fg = '$want_fg'
Actual: '$got_fg'"
	[ "$got_bg" = "$want_bg" ] || fail "Expected: cell($row,$col) bg = '$want_bg'
Actual: '$got_bg'"
}

trap cleanup EXIT
cleanup

require_cmd tty
require_cmd sed

TTY=$(tty 2>/dev/null) || fail "tty command failed"
case "$TTY" in
/dev/*)
	:
	;;
*)
	fail "Expected: tty path under /dev
Actual: '$TTY'"
	;;
esac

printf '\033c' > "$TTY"
printf 'AB\bC\tD' > "$TTY"
snapshot
expect_cell 0 0 41 ffffffff ff000000
expect_cell 0 1 43 ffffffff ff000000
expect_cell 0 2 20 ffffffff ff000000
expect_cell 0 8 44 ffffffff ff000000
expect_meta cursor_row 0
expect_meta cursor_col 9

printf '\033c' > "$TTY"
printf '\033[31;44mR\033[0mW' > "$TTY"
snapshot
expect_cell 0 0 52 ffff0000 ff0000ff
expect_cell 0 1 57 ffffffff ff000000
expect_meta fg ffffffff
expect_meta bg ff000000

printf '\033c' > "$TTY"
printf 'A\033[s\033[2;5HZ\033[uY\033[?25l' > "$TTY"
snapshot
expect_cell 0 0 41 ffffffff ff000000
expect_cell 0 1 59 ffffffff ff000000
expect_cell 1 4 5a ffffffff ff000000
expect_meta cursor_row 0
expect_meta cursor_col 2
expect_meta cursor_hidden 1
printf '\033[?25h' > "$TTY"
snapshot
expect_meta cursor_hidden 0

printf '\033c' > "$TTY"
printf 'ABCDE\r\033[2P\033[2@XY' > "$TTY"
snapshot
expect_cell 0 0 58 ffffffff ff000000
expect_cell 0 1 59 ffffffff ff000000
expect_cell 0 2 43 ffffffff ff000000
expect_cell 0 3 44 ffffffff ff000000
expect_cell 0 4 45 ffffffff ff000000

printf '\033c' > "$TTY"
printf '\033[1;1HA\033[2;1HB\033[3;1HC\033[2;1H\033[LX' > "$TTY"
snapshot
expect_cell 0 0 41 ffffffff ff000000
expect_cell 1 0 58 ffffffff ff000000
expect_cell 2 0 42 ffffffff ff000000
expect_cell 3 0 43 ffffffff ff000000

printf '\033c' > "$TTY"
printf '\033[1;1HA\033[2;1HB\033[3;1HC\033[2;1H\033[M' > "$TTY"
snapshot
expect_cell 0 0 41 ffffffff ff000000
expect_cell 1 0 43 ffffffff ff000000
expect_cell 2 0 20 ffffffff ff000000

printf '\033c' > "$TTY"
printf 'T\033[2;4r\033[2;1HL1\033[3;1HL2\033[4;1HL3\033[4;1H\nB' > "$TTY"
snapshot
expect_meta scroll_top 1
expect_meta scroll_bot 3
expect_cell 0 0 54 ffffffff ff000000
expect_cell 1 0 4c ffffffff ff000000
expect_cell 1 1 32 ffffffff ff000000
expect_cell 2 0 4c ffffffff ff000000
expect_cell 2 1 33 ffffffff ff000000
expect_cell 3 0 42 ffffffff ff000000

printf '\033c' > "$TTY"
printf 'M\033[?1049hA\033[?7l' > "$TTY"
snapshot
expect_meta alt_active 1
expect_meta no_wrap 1
expect_cell 0 0 41 ffffffff ff000000

cols=$(meta_get cols) || fail "missing meta key: cols"
printf '\033[1;%dHZQ\033[?7h\033[?1049l' "$cols" > "$TTY"
snapshot
last_col=$((cols - 1))
expect_meta alt_active 0
expect_meta no_wrap 0
expect_cell 0 0 4d ffffffff ff000000
expect_cell 0 "$last_col" 20 ffffffff ff000000
