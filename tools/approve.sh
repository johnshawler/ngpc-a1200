#!/bin/bash
# Usage: tools/approve.sh class perpixel | exempt <rule> <reason...>   (plan: no longer needed; the user dropped it)
# FOR THE USER ONLY. A model working on this repository must never run this script or write APPROVALS (CLAUDE.md).
# Records your approval in APPROVALS, which tools/check_rules.py reads:
#   plan              approve FEASIBILITY.md as it is now (any later change needs a new approval)
#   class perpixel    allow the per-pixel class (soft gates) for this plan
#   exempt <rule> ... allow one of the brief's "never do these" (interpreter, float), with your reason
# Commit APPROVALS afterwards.
set -e
cd "$(dirname "$0")/.."
[ -f FEASIBILITY.md ] || { echo "approve: no FEASIBILITY.md"; exit 1; }
SHA=$(sha256sum FEASIBILITY.md | cut -d' ' -f1)
case "$1" in
  plan) LINE="plan $SHA" ;;
  class) [ "$2" = perpixel ] || { echo "approve: class perpixel"; exit 1; }; LINE="class perpixel $SHA" ;;
  exempt) [ -n "$2" ] && [ -n "$3" ] || { echo "approve: exempt <rule> <reason>"; exit 1; }; shift; LINE="exempt $*" ;;
  *) echo "usage: tools/approve.sh plan | class perpixel | exempt <rule> <reason>"; exit 1 ;;
esac
echo "$LINE    # $(date -u +%Y-%m-%d)" >> APPROVALS
echo "approve: added to APPROVALS: $LINE"
