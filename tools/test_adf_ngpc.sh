#!/bin/bash
# Usage: tools/test_adf_ngpc.sh <ticks> [seconds] [screenshot.png|-] [fs-uae options...]
# Proves a floppy image of the port: makes one with tools/make_adf_ngpc.sh whose Startup-Sequence runs
# "<name> bench <ticks>", boots it on the emulated A1200 with 4 MB fast RAM and nothing else to boot from, waits
# until bench.txt is on the floppy (the program writes it there), prints it and the host test's checksum for the
# same ticks. KICKSTART=<file> boots a Kickstart ROM the user supplied (original/*.rom, never committed) instead of
# the free AROS ROM.
set -e
cd "$(dirname "$0")/.."
. ./port.conf
N="$1"; SECS="${2:-900}"; SHOT="${3:--}"; shift 3 2>/dev/null || shift $#
W=$(mktemp -d /tmp/test_adf.XXXXXX)
ADF="$W/test.adf"
tools/make_adf_ngpc.sh "$ADF" "$NAME bench $N" | grep -E "make_adf|used:"
mkdir -p "$W/nohd"
KS=""; [ -n "$KICKSTART" ] && KS="--kickstart_file=$(realpath "$KICKSTART")"
( for i in $(seq 1 $(( SECS / 10 ))); do                 # bench.txt on the floppy: the run is over
    sleep 10
    if xdftool "$ADF" read bench.txt "$W/bench.txt" >/dev/null 2>&1 && grep -q checksum "$W/bench.txt" 2>/dev/null; then
      touch "$W/done"; break
    fi
  done ) &
POLL=$!
START=$(date +%s)
UAE_DONE="$W/done" UAE_SHOT_AT="${SHOT_AT:-60}" tools/run_uae.sh "$W/nohd" "$SECS" "$SHOT" --fast_memory=4096 \
  --floppy_drive_0="$ADF" --writable_floppy_images=1 --hard_drive_0= $KS "$@"
kill $POLL 2>/dev/null || true
echo "test_adf: the emulator ran $(( $(date +%s) - START )) s (${KICKSTART:-AROS ROM})"
if [ -s "$W/bench.txt" ]; then cat "$W/bench.txt"; else echo "test_adf: no bench.txt on the floppy"; fi
HT="$W/host_test"
cc -O2 $HOST_CFLAGS $HOST_TEST -o "$HT" && echo "host: $("$HT" "$N" | tail -1)"
rm -rf "$W"
