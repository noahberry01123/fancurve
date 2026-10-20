#!/bin/bash
# Compile the SwiftUI menu-bar app into FanCurve.app.
set -e
cd "$(dirname "$0")"

APP="FanCurve.app"
BIN="$APP/Contents/MacOS/FanCurve"

echo "compiling C..."
clang -c -O2 fanapi.c smc.c

echo "compiling Swift + assembling bundle..."
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp gui/Info.plist "$APP/Contents/Info.plist"

swiftc -O -parse-as-library \
    gui/FanCurve.swift fanapi.o smc.o \
    -import-objc-header gui/Bridge.h \
    -framework SwiftUI -framework AppKit \
    -framework IOKit -framework CoreFoundation \
    -o "$BIN"

# Ad-hoc sign so the app runs locally without Gatekeeper complaints.
codesign --force --sign - "$APP" 2>/dev/null || true

rm -f fanapi.o smc.o
echo "built $APP"
