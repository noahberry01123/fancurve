#!/bin/bash
# One-command setup for FanCurve:
#   builds the daemon + app, installs them, starts the controller, opens the app.
# You'll be asked for your password once (the daemon needs root to drive the SMC).
set -e
cd "$(dirname "$0")"

PLIST="com.fancurve.daemon.plist"
DAEMON="/Library/LaunchDaemons/$PLIST"
CONFDIR="/Library/Application Support/FanCurve"

echo "==> Building daemon..."
make fancurved

echo "==> Building app..."
./build_app.sh

# Stop Macs Fan Control so the two don't fight over the SMC.
if pgrep -qi "Macs Fan Control"; then
    echo "==> Quitting Macs Fan Control..."
    osascript -e 'quit app "Macs Fan Control"' 2>/dev/null || true
fi

echo "==> Installing (needs your password)..."
# Config directory: created by root, owned by you so the app can write to it.
sudo mkdir -p "$CONFDIR"
sudo chown "$(id -un)" "$CONFDIR"

# Seed a default config if none exists.
[ -f "$CONFDIR/config" ] || printf 'mode auto\npercent 50\nlow 55\nhigh 90\n' > "$CONFDIR/config"

# Daemon binary + launchd job.
sudo install -m 755 fancurved /usr/local/bin/fancurved
sudo install -m 644 "$PLIST" "$DAEMON"
sudo launchctl bootout system "$DAEMON" 2>/dev/null || true
sudo launchctl bootstrap system "$DAEMON"

# App into /Applications.
rm -rf "/Applications/FanCurve.app"
cp -R FanCurve.app "/Applications/FanCurve.app"

echo "==> Launching app..."
open "/Applications/FanCurve.app"

echo ""
echo "Done. Look for the fan icon in your menu bar."
echo "  • Daemon:  $DAEMON  (logs: /var/log/fancurve.log)"
echo "  • Config:  $CONFDIR/config"
echo "  • Uninstall:  ./uninstall.sh"
