# Game files: what setup does, and how to do it by hand

Players only need `setup.bat` / `setup.sh` ([INSTALL.md](../INSTALL.md)). This page describes what `tools/setup_tl2.py`
does with your dumps, for developers and for troubleshooting.

## Inputs

| Title | Id | Version | Needed for |
|---|---|---|---|
| Torchlight II | `010090400D366000` | v0 | RomFS (base of the BKTR patch) |
| Update (release 5) | `010090400D366800` | **v327680** | ExeFS (`main` build id `a0c6c2d22dea7933bccce074c54e1b0f`, `sdk` `3ae34e689d7a35f13cbf42bfd672af8b`) + the BKTR RomFS patch |
| `prod.keys` | — | — | decrypting the packages (your own console's) |

Packages may be NSP, NSZ, XCI or XCZ, also split dumps. They are identified by their content meta (CNMT), not by the file
name. All game languages are in the base RomFS (`CONSOLELOC/*.LANGUAGE`). The Yapper pet DLC (`010090400D367001`) is not
used yet and is skipped.

## Output (`work/Torchlight2Mobile`, copied to `Download/Torchlight2Mobile` on the phone)

```text
Torchlight2Mobile/
  romfs/{PAKS,PROGRAMS,MUSIC,MOVIES,CONSOLELOC}/..., SPLASH.XTX, VERSION.TXT   the merged RomFS (257 files, ~1.9 GiB)
  dm/out/{main,sdk}.{patched.bin,fixups.tsv,gates.tsv,sites.tsv,...}           the patched modules
  dm/shaders/<key>.xgs + _vk_build.json                                        64 SPIR-V bundles (~5 MB)
  mods/README.txt                                                              where PC .MOD files go (empty otherwise)
```

The app also reads `dm/env` (KEY=VALUE lines, debugging switches) and `drivers/` (Vulkan driver packages) there, and
writes `logs/` and `saves/` (save export).

## Steps

| # | Step | Tool | Result |
|---|---|---|---|
| 3 | ExeFS of the update | `content_reader.py` (plain NSP / XCI) or `extract_exefs_nsz.py` (NSZ / XCZ); by hand: `tools/exefs_nsp.py <update> work/exefs-update` | `work/exefs-update/{main, sdk, subsdk0, rtld, main.npdm}`; the build ids are checked |
| 4 | Offline patch | `DM_TARGET=android python dm/tools/patch_nso.py work/exefs-update/<mod> <out> <mod>` for `main`, `sdk` | `dm/out/*` (main: 6550 sites, sdk: 11277 sites + 93 host gates). `subsdk0` (the movie library) is not loaded: the host serves it |
| 5 | Patch check | `python dm/verify/equiv.py <out> <mod> work/exefs-update/<mod> --jobs N` | every rewritten site against the original instruction under random states, plus a 300-mutation self-check; must print `EQUIV <mod> OK`. Needs `unicorn` |
| 6 | RomFS | `tools/romfs_d3.py extract <base> <update> <dest>` (the generic base + BKTR reader; NSZ / XCZ: program expanded first with `tools/ncz2nsp.py`) | IVFC hashes verified, BKTR applied, files + `work/romfs-manifest.json` (sha256 of every file) |
| 7 | Shaders | `tools/tl2_shader_jobs.py <romfs>/PROGRAMS work/shaders-jobs` → `tools/build_dmsh_xlate_d3.py` (.NET 10 SDK, Ryujinx 1.3.2 from `dm/third_party/ryujinx`) → `XLATE=<dmsh_xlate> python dm/gfx/tools/dmsh_vk_xc2.py work/shaders-jobs <dm/shaders>` | 64 stages (28 vertex, 32 pixel, 4 compute skinning) → `dm/shaders/*.xgs` |

Every step is skipped when its output is complete; delete an output to redo it. `setup --steps exefs,patch,verify` (etc.)
runs a subset, `--skip-verify` / `--skip-shaders` leave those out, `--keep-nsp` keeps the expanded NSPs, `--work DIR`
moves the whole work folder. The results are deterministic: the same dumps give byte-identical patched modules, RomFS
files and shader bundles (checked: a fresh setup run reproduces the files of the development phone byte for byte).

SPIRV-Tools are optional: without `spirv-opt` the capability trim is done in Python (same output), without `spirv-val`
the per-shader validation is skipped. Setup finds them on `PATH`, through `SPIRV_OPT` / `SPIRV_VAL`, or in an Android NDK
(`ANDROID_NDK_HOME`, `ANDROID_HOME/ndk/*`).

## Copying to the phone

`setup push [serial]` copies only missing or size-mismatched files over adb (an interrupted copy resumes; files in
`mods/` on the phone are never overwritten) and grants the app *All files access*. By hand:

```sh
adb shell mkdir -p /sdcard/Download/Torchlight2Mobile/dm
adb push work/Torchlight2Mobile/romfs /sdcard/Download/Torchlight2Mobile/
adb push work/Torchlight2Mobile/dm/out /sdcard/Download/Torchlight2Mobile/dm/
adb push work/Torchlight2Mobile/dm/shaders /sdcard/Download/Torchlight2Mobile/dm/
adb push work/Torchlight2Mobile/mods /sdcard/Download/Torchlight2Mobile/
adb shell appops set org.torchlight2.port MANAGE_EXTERNAL_STORAGE allow          # after installing the APK
```

Large files over a flaky USB / wireless adb link: `tools/push_phone.sh` (and `tools/push_big.sh`) resume in chunks.

## PC mods

`.MOD` files (GUTS) in `Download/Torchlight2Mobile/mods/` are listed in the gear dialog (*Mods*). Ticked mods are
converted on the phone at the next start into extra data packs (`files/tl2mods/overlay/PAKS/ZMOD_<nn>_<title>.PAK/.MAN`,
textures to the Switch XTX format, cached by size + time) that the host puts above the RomFS; lower in the list = loaded
later = wins. While a mod is on, saves go to a separate folder seeded from the normal one. On the PC the same conversion
is `python tools/tl2_mod.py convert <mod.MOD> <out dir>` (byte-identical to the app's converter; `tools/tl2_mod_java_test.py`
checks that). Details: [TL2_PORT.md](TL2_PORT.md), section "Моды PC".

## Developer tools (not needed to play)

| Tool | What |
|---|---|
| `tools/nso_symbols.py <exefs dir> main sdk` | decompressed images (`*.raw`) and import / export tables (`*.symbols.json`) |
| `tools/symbt.py <log> [exefs dir]`, `tools/plt.py`, `tools/xref_str.py`, `tools/ipc_reach.py` | backtrace symbolisation, PLT names, string xrefs, SDK IPC reachability |
| `tools/tl2_mod.py selftest / convert` | PAK / MAN reader-writer and texture converter |
| `tools/run.sh`, `hangcheck.sh`, `shots.sh`, `keep_awake.sh`, `rebuild.sh` | device test runs (`ADB_SERIAL`, `ADB`), logs to `work/logs/` |
