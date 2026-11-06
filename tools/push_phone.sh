#!/bin/bash
# Push everything Torchlight II Mobile needs to a phone (default the USB phone emulator-5554) without starting the game:
# patched modules (../work/patched -> dm/out), shaders (../work/shaders-vk -> dm/shaders), the RomFS (../work/romfs -> romfs, 257 files,
# 1.9 GiB), the Turnip 27 package and the APK. Layout: /sdcard/Download/Torchlight2Mobile/{romfs,dm,drivers}. Files already there with the
# right size are skipped (run it again after an interruption). Never kills the adb server.
export MSYS_NO_PATHCONV=1
S=${ADB_SERIAL:-emulator-5554}; export ADB_SERIAL=$S
ADB="${ADB:-adb} -s $S"
R=/sdcard/Download/Torchlight2Mobile; W=${TL2_WORK:-work}
cd "$(dirname "$0")/.."
rsize() { $ADB shell "stat -c %s '$1' 2>/dev/null" | tr -d '\r'; }
big() {   # <local> <remote>
  local want=$(stat -c %s "$1") have=$(rsize "$2")
  [ "$have" = "$want" ] && return
  if [ -n "$have" ] && [ "$have" -gt 0 ] 2>/dev/null && [ "$want" -gt 268435456 ]; then bash tools/push_big.sh "$1" "$2" 256 | tail -1; return; fi
  $ADB push "$1" "$2" 2>&1 | tail -1
  [ "$(rsize "$2")" = "$want" ] || { echo "plain push failed, chunks: $2"; $ADB shell "rm -f '$2'"; bash tools/push_big.sh "$1" "$2" 256 | tail -1; }
}
$ADB shell "mkdir -p $R/romfs $R/dm/out $R/dm/shaders $R/drivers $R/logs"
(cd $W/patched && $ADB push . $R/dm/out/ 2>&1 | tail -1)
(cd $W/shaders-vk && $ADB push . $R/dm/shaders/ 2>&1 | tail -1)
$ADB push ${DRIVER:-work/driver-assets/drivers/turnip27} $R/drivers/ 2>&1 | tail -1
$ADB install -r src/work/TL2-port-debug.apk 2>&1 | tail -1
$ADB shell appops set org.torchlight2.port MANAGE_EXTERNAL_STORAGE allow
for f in $(cd $W/romfs && find . -type f -printf '%s %P\n' | sort -n | cut -d' ' -f2-); do
  $ADB shell "mkdir -p '$R/romfs/$(dirname "$f")'" < /dev/null; big "$W/romfs/$f" "$R/romfs/$f" < /dev/null; done
echo "checks:"
$ADB shell "find $R/romfs -type f | wc -l; ls $R/dm/out | wc -l; ls $R/dm/shaders | wc -l; du -sh $R"
echo "expected: $(cd $W/romfs && find . -type f | wc -l) romfs files, $(ls $W/patched | wc -l) out files, $(ls $W/shaders-vk | wc -l) shader files"
