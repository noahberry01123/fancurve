#!/bin/bash
# Remove FanCurve: stop + delete the daemon, remove the app and config.
set -e
cd "$(dirname "$0")"

PLIST="com.fancurve.daemon.plist"
DAEMON="/Library/LaunchDaemons/$PLIST"

echo "==> Removing (needs your password)..."
sudo launchctl bootout system "$DAEMON" 2>/dev/null || true
sudo rm -f "$DAEMON" /usr/local/bin/fancurved
sudo rm -rf "/Library/Application Support/FanCurve"
rm -rf "/Applications/FanCurve.app"

# Quit the menu-bar app if it's running.
osascript -e 'quit app id "com.fancurve.app"' 2>/dev/null || true

echo "Done. Fans are back under macOS control."
