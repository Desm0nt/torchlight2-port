# Pipeline warm-up seeds (Torchlight II)

`<family>.bin` files in this folder are packed into the APK as assets `warm-seed/<family>.bin` by `tools/build_apk.ps1`
(empty files and a missing folder are skipped). `<family>` is the engine's GPU policy family:
`adreno`, `mali`, `powervr`, `xclipse`, `maleoon`, `other` (boot.log: `NVN gpu family=... policy family=...`).

At every start `DmActivity.unpackWarmSeeds` copies them to `files/warm-seed/` and the engine (`xbg_game.cpp`, `warm::start`)
merges the seed of the phone's family behind the phone's own `dm/vk-pipeline-list.bin`. The warm-up threads then build those
pipelines during the boot, so a first launch and places the player has not visited yet do not compile them inside frames.
Records a phone cannot build fail in the warm-up (`warm_failed`) and are otherwise harmless.

`mali.bin` (293 records) is `work/warm-mali/vk-pipeline-list-mali-sim.bin` from the Mi 14 Ultra run with `DM_GFX_SIM_GPU=mali`
(16.11, Temple Steppes + inventory, no fights): source 2 below. Its keys have not been compared with a real Mali's list yet.
Every record uses only state a Mali reports (vertex formats R8G8B8A8_UNORM / R32G32(B32(A32))_SFLOAT, attachments RGBA8 / D16 / D32,
fill mode, no depth bounds / logic op / dual-source blend, dynamic viewport + scissor), so a non-matching record costs a background
build, not invalid API use.

## Making a seed

A seed is one or more phones' `vk-pipeline-list.bin` merged (same file format, de-duplicated by key):

```
python dm/android/tools/merge_warm_lists.py -o dm/android/tl2/warm-seed/mali.bin <listA> <listB> ...
```

Where the lists come from:

1. A player's phone of that family: `Download/Torchlight2Mobile/dm/vk-pipeline-list.bin` (ask for it together with the log).
   This is the best source: it is exactly what that driver built.
2. Our Adreno phone running the Mali policy: put `DM_GFX_SIM_GPU=mali` in `dm/env`, delete `dm/vk-pipeline-list.bin`, play
   through as much as possible (every class, towns, fights, portals, menus), then pull the list. Restore `dm/env` afterwards
   and delete the list again (it holds Mali-policy records the Adreno path has no use for).
   Whether such a list matches a real Mali's keys is not known yet; check it against a player's list before trusting it:

```
python dm/android/tools/merge_warm_lists.py --compare <sim list> <real Mali list>
```

A seed only matches the shader bundles and the engine build it was recorded with (the key holds shader hashes, the descriptor
layout and the render pass): after a change to the translator or to pipeline state, record it again. A stale seed costs
background build time for records that fail or are never used, nothing else.

The files hold shader keys and Vulkan pipeline state only, no game data. `dm/.gitignore` ignores `*.bin` except here.

## Checking on a phone

boot.log:

```
NVN warm seed <path>/mali.bin new=<N>
NVN warm list=<own+N> enabled=1 own=<own> seed=<N> family=mali (DM_GFX_WARM_SEED=1 default; ...)
NVN PERF pipelines_new=... warm_hits=... created=... warm_built=<grows to the list size> warm_failed=...
```

`created` (pipelines built inside frames) should stay near 0 in places the seed covers. `DM_GFX_WARM_SEED=0` (or a marker file
`dm/gfx-warm-seed` containing `0`) turns the merge off for an A/B run.
