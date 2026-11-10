#!/bin/bash
# Native + APK rebuild and install on the phone (ADB_SERIAL, default emulator-5554)
cd "$(dirname "$0")/.."
export GRADLE=${GRADLE:-gradle}
timeout 600 powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_apk.ps1 < /dev/null 2>&1 | tail -n 2
timeout 120 adb -s ${ADB_SERIAL:-emulator-5554} install -r work/TL2-port-debug.apk 2>&1 | tail -1
