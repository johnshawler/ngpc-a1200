#!/bin/bash
# Usage: tools/long_bench.sh <dist-dir> <exe-name> <ticks> <max-seconds> [screenshot.png|-] [extra fs-uae options...]
# Like tools/bench.sh, with a time limit you choose. tools/bench.sh allows ticks / 25 + 60 seconds, which is right
# for a port that keeps game time; a port that is still far too slow (its first, pixel-by-pixel display, code not
# yet optimised) needs longer to play the same number of ticks. For looking at such a port, not for the referee.
# KEEP=<dir> copies what the program wrote (bench.txt and any other file) there.
DIST="$(realpath "$1")"; EXE="$2"; TICKS="$3"; SECS="$4"; SHOT="${5:--}"; shift 5 2>/dev/null || shift $#
D=$(mktemp -d /tmp/uaebench.XXXXXX)
cp -r "$DIST"/. "$D"/; mkdir -p "$D/S"; printf '%s bench %s\n' "$EXE" "$TICKS" > "$D/S/startup-sequence"
rm -f "$D/bench.txt"
UAE_DONE="$D/bench.txt" UAE_SHOT_AT="${SHOT_AT:-30}" "$(dirname "$0")/run_uae.sh" "$D" "$SECS" "$SHOT" "$@"
if [ -e "$D/bench.txt" ]; then cat "$D/bench.txt"; else echo "NO bench.txt (crashed, hung, or did not finish in $SECS s)"; fi
if [ -n "$KEEP" ]; then mkdir -p "$KEEP"; cp -r "$D"/. "$KEEP"/; fi
rm -rf "$D"
