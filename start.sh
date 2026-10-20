#!/bin/bash
# Build (if needed), stop Macs Fan Control, and start fancurve.
#
#   ./start.sh                 run the temperature curve (default 55 90 3)
#   ./start.sh curve 50 85 2   run the curve with custom LOW HIGH INTERVAL
#   ./start.sh set 0 4000      force a fan to a fixed rpm, then exit
#   ./start.sh list            just show the fans (no sudo, no changes)
#
# You'll be asked for your password once (the SMC writes need root).

set -e
cd "$(dirname "$0")"

# 1. Build if the binary is missing or out of date.
if [ ! -x ./fancurve ] || [ smc.c -nt ./fancurve ] || [ main.c -nt ./fancurve ]; then
    echo "building fancurve..."
    make
fi

# Read-only commands need no sudo and change nothing.
case "${1:-curve}" in
    list|dump|get)
        exec ./fancurve "$@"
        ;;
esac

# 2. Quit Macs Fan Control so the two don't fight over the SMC.
if pgrep -qi "Macs Fan Control"; then
    echo "quitting Macs Fan Control..."
    osascript -e 'quit app "Macs Fan Control"' 2>/dev/null || true
    sleep 1
fi

# 3. Run fancurve as root. Default to the curve if no command was given.
if [ "$#" -eq 0 ]; then
    set -- curve
fi

echo "starting: fancurve $*"
echo "(Ctrl-C stops it and returns the fans to auto)"
exec sudo ./fancurve "$@"
