#!/bin/bash
# Usage: tools/calib_m1230.sh            prints the --uae_cpu_throttle value for tools/port.sh m1230 on this host
# FS-UAE cannot run a cycle-exact 68030 at 33 MHz, and its CPU throttle is set against the host's own speed: the same
# value gave 25 and 90 late blanks on two hosts. So the throttle is found on each host, from a fixed workload that is
# not the game (tools/calib68k.c): its time on the cycle-exact A1200 with fast RAM (the brief's reference machine), divided
# by 2.7 (the brief: a 50 MHz 68030 with fast RAM is about 6x a stock A1200 for logic, fast RAM alone 1.5x; 33 MHz about
# 2.7x the A1200 with fast RAM), is the time the throttled 68030 must take. Within 4%, by bisection.
set -e
cd "$(dirname "$0")/.."
[ -s dist/calib/calib68k ] || { mkdir -p dist/calib; CFLAGS="-Ikit" tools/build.sh dist/calib/calib68k kit/sys.c tools/calib68k.c >/dev/null; }
run() {                                         # run(fs-uae options...) -> the workload's colour clocks
  local D; D=$(mktemp -d /tmp/uaecalib.XXXXXX)
  cp dist/calib/calib68k "$D/"; mkdir -p "$D/S"; echo calib68k > "$D/S/startup-sequence"
  UAE_DONE="$D/calib.txt" tools/run_uae.sh "$D" 90 - "$@" >/dev/null 2>&1 || true
  sed -n 's/^calib_cck=\([0-9]*\).*/\1/p' "$D/calib.txt" 2>/dev/null; rm -rf "$D"
}
CPU="--fast_memory=4096 --cpu=68030 --uae_cpu_cycle_exact=false --uae_cpu_compatible=true --uae_cpu_speed=max --uae_blitter_cycle_exact=true"
REF=$(run --fast_memory=4096)
[ -n "$REF" ] || { echo "calib: the reference run wrote no calib.txt" >&2; exit 1; }
WANT=$(( REF * 10 / 27 ))
LO=100; HI=950                                  # throttle -LO is too fast, -HI too slow (checked below)
for i in 1 2 3 4 5 6 7 8; do
  MID=$(( (LO + HI) / 2 )); T=$(run $CPU --uae_cpu_throttle=-$MID)
  echo "calib: throttle -$MID: $T colour clocks (reference $REF, want $WANT)" >&2
  [ -n "$T" ] || { echo "calib: no result at -$MID" >&2; exit 1; }
  if [ $(( T * 100 )) -lt $(( WANT * 96 )) ]; then LO=$MID; elif [ $(( T * 100 )) -gt $(( WANT * 104 )) ]; then HI=$MID; else break; fi
done
echo "$MID"
