# Game-derived information in this repository

Torchlight II Mobile contains **no game code and no game data**: no executables or parts of them, no RomFS files (PAKs,
`.NVN` shaders, music, movies, texts), no translated `.xgs` bundles, no textures, art, audio, video, text or fonts, and no
keys. The player supplies the game ([GAME_FILES.md](GAME_FILES.md)) and every game-derived output (ExeFS, patched
modules, extracted RomFS, shader jobs, translated `.xgs` bundles) is produced on the player's machine into the
git-ignored `work/` directory. The app's launcher icon (`dm/android/tl2/icon.png`) is original artwork made for this
repository (a brass "II" with a torch flame), not the game's icon or logo.

To make the original code run on Android, the repository does contain *facts about* the game, found by analysing the
author's own copy: addresses, short instruction encodings, structure offsets, identifiers and format descriptions. This
page lists them so their extent is clear. They apply only to Torchlight II for Switch, update v327680 (`main` build id
`a0c6c2d22dea7933…`, `sdk` build id `3ae34e689d7a35f1…`, SDK 8.2.0); the host checks the original instruction words
before it changes anything, and setup checks the build ids.

| What | Where | Extent |
|---|---|---|
| **Patcher site form**: one jump-table form of `main` (`adrp x18` + `add`, `ldrsw`, `br`) that the offline rewriter accepts | `dm/tools/patch_nso.py` | Instruction patterns, no code |
| **Widescreen sites**: offsets and original instruction words of the window / video-settings sizes and the display-mode snap, rewritten at load time; camera viewport aspect | `dm/host/hle_tl2_wide.c` | ~10 instruction words, each checked before it is replaced |
| **Hybrid UI layout**: addresses of the console UI's add-child / layout functions, the screen-layer and root fields, the call sites that keep the full width (HUD) or go into the centred 16:9 band | `dm/host/hle_tl2_wide.c` | Offsets and return addresses; the game's own list code is re-implemented in C |
| **UI reading for touch**: the console UI tree (manager, element rects, lists, buttons, radial menus, panels, dialog, title screen), their focus / selection / accept entry points and the input dispatcher | `dm/host/hle_tl2_ui.c` | Offsets and entry points; taps call the game's own functions |
| **HUD action bar** (slot layout, bound skill hash, icon element, cooldown fill) for the skill icons on the touch buttons | `dm/host/hle_tl2_icons.c` | Offsets only; the icons are copied from the running game's own picture on the phone, nothing is stored |
| **Automap setting** (the minimap / big map mode field and its setter) | `dm/host/hle_tl2_map.c` | Two field ids and one entry point |
| **Mod support facts**: PAK / MAN v2 layout, the console texture container (XTX) and its formats, the stubbed engine mod loader (one address), the pack load order | `tools/tl2_mod.py`, `dm/android/app/.../Tl2ModConv.java`, `dm/host/hle_fs.c` | Format description and converters; no game files |
| **SDK 8.2 internals**: overloads of nnSdk functions the host serves (audio out, touch screen, account, fs save data, movie, ldn, nifm) | `dm/host/hle_tl2.c`, `hle_movie.c`, shared files | Symbol names (from the exports of the Switch modules) and constants |
| **Shader container layout** (`PROGRAMS/*.NVN`: GLSLCoutput sections, program pairing from `CONSOLE_SHADERS.PROGRAM`), NVN depth mode set at a quoted address | `tools/tl2_shader_jobs.py` | Parser; shader identifiers (16-hex-digit keys) appear in notes |
| **Analysis notes**: function names, addresses, call chains, measurements | `docs/tl2/TL2_PORT.md` | Text analyses |

Function and type names come from the symbols and RTTI the Switch modules carry (or are descriptive names given by the
author).

What the repository does **not** contain, and the tooling never writes into the source tree: `*.nsp / *.nsz / *.nca /
*.xci`, ExeFS (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`), `*.patched.bin`, RomFS files (`*.PAK`, `*.MAN`, `*.NVN`,
`*.XTX`…), shader jobs (`*.rec`, `jobs.tsv`, `shader_map.tsv`…), `.xgs` bundles, saves, mods, console keys.

The tree also carries the records of the sibling ports built on the same host: Diablo III (`DM_GAME_D3`, described in
[../d3/DERIVED.md](../d3/DERIVED.md); Torchlight II uses its SDK 8.2 / NVN 53 code but not its game-specific sites), DOOM
(2016) patch sites and gates (`DM_GAME_DOOM` code) and the DOOM Eternal / Xenoblade 2 code paths, each bound to its own
game's build id. The base d2r-mobile tree carries its own D2R-derived tables; they are described in
[`docs/DERIVED.md`](../DERIVED.md) and are not used by the Torchlight II build.
