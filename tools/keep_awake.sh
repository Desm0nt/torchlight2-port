#!/bin/bash
# Keep the phone's screen from timing out while the owner is away: a harmless key event (Scroll Lock, ignored by the app) every 50 s counts as user
# activity. No settings are changed; stop with: touch work/stop_awake
export MSYS_NO_PATHCONV=1
rm -f work/stop_awake
while [ ! -f work/stop_awake ]; do adb -s ${ADB_SERIAL:-emulator-5554} shell input keyevent 116 >/dev/null 2>&1; sleep 50; done
