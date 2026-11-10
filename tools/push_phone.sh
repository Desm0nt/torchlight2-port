#!/bin/bash
# Push everything Torchlight II Mobile needs to a phone without starting the game: patched modules (-> dm/out), shaders
# (-> dm/shaders), the RomFS (-> romfs: 257 files, 1.9 GiB), the mods folder, the Turnip 27 adrenotools package (also bundled in
# the APK) and the APK. Layout on the phone: /sdcard/Download/Torchlight2Mobile/{romfs,dm,mods,drivers}.
# Sources (defaults = the folder tools/setup_tl2.py prepares, work/Torchlight2Mobile):
#   PREP=<dir> (default work/Torchlight2Mobile) with romfs/, dm/out/, dm/shaders/, mods/; APK=<file> (default work/TL2-port-debug.apk);
#   DRIVER=<dir> (default work/driver-assets/drivers/turnip27, from tools/fetch_turnip.py). Device: ADB_SERIAL / ADB as in tools/run.sh.
# Big files: plain adb push first, tools/push_big.sh (resumable 256 MiB chunks) when it fails or the size is wrong; files already on
# the phone with the right size are skipped, so the script can simply be run again after an interruption. Never kills the adb server.
# (setup push [serial] does the same for the data folder without the APK / driver.)
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
export ADB_SERIAL=${ADB_SERIAL:-emulator-5554}
ADB="${ADB:-adb} -s $ADB_SERIAL"
R=/sdcard/Download/Torchlight2Mobile
P=${PREP:-work/Torchlight2Mobile}; APK=${APK:-work/TL2-port-debug.apk}; DRV=${DRIVER:-work/driver-assets/drivers/turnip27}
rsize() { $ADB shell "stat -c %s '$1' 2>/dev/null" | tr -d '\r'; }
big() {   # <local> <remote>
  local want=$(stat -c %s "$1") have=$(rsize "$2")
  [ "$have" = "$want" ] && return
  if [ -n "$have" ] && [ "$have" -gt 0 ] 2>/dev/null && [ "$want" -gt 268435456 ]; then bash tools/push_big.sh "$1" "$2" 256 | tail -1; return; fi
  $ADB push "$1" "$2" 2>&1 | tail -1
  [ "$(rsize "$2")" = "$want" ] || { echo "plain push failed, chunks: $2"; $ADB shell "rm -f '$2'"; bash tools/push_big.sh "$1" "$2" 256 | tail -1; }
}
$ADB shell "mkdir -p $R/romfs $R/dm/out $R/dm/shaders $R/drivers $R/mods $R/logs"
(cd "$P/dm/out" && $ADB push . $R/dm/out/ 2>&1 | tail -1)
(cd "$P/dm/shaders" && $ADB push . $R/dm/shaders/ 2>&1 | tail -1)
[ -f "$P/mods/README.txt" ] && $ADB push "$P/mods/README.txt" $R/mods/ 2>&1 | tail -1
[ -d "$DRV" ] && $ADB push "$DRV" $R/drivers/ 2>&1 | tail -1
[ -f "$APK" ] && $ADB install -r "$APK" 2>&1 | tail -1
$ADB shell appops set org.torchlight2.port MANAGE_EXTERNAL_STORAGE allow
# RomFS: smallest files first, each resumable (a for loop, not "while read": adb shell would eat the list from stdin; no spaces in names)
for f in $(cd "$P/romfs" && find . -type f -printf '%s %P\n' | sort -n | cut -d' ' -f2-); do
  $ADB shell "mkdir -p '$R/romfs/$(dirname "$f")'" < /dev/null; big "$P/romfs/$f" "$R/romfs/$f" < /dev/null; done
echo "checks:"
$ADB shell "find $R/romfs -type f | wc -l; ls $R/dm/out | wc -l; ls $R/dm/shaders | wc -l; du -sh $R"
echo "expected: $(cd "$P/romfs" && find . -type f | wc -l) romfs files, $(ls "$P/dm/out" | wc -l) out files, $(ls "$P/dm/shaders" | wc -l) shader files"
