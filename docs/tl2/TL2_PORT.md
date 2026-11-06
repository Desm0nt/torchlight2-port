# Torchlight II (Switch) → нативный Android: состояние и журнал

Подход тот же, что у DOOM 2016 / Eternal / XC2 / Diablo III: ARM64-код игры исполняется напрямую, NSO патчится офлайн
(x18/TLS/SVC/cntpct), `nn::` — HLE, NVN → Vulkan, шейдеры офлайн. Дерево — клон публичного D3-репо (`d3-port-repo`, main 0545886),
ветка `tl2`; сборка `DM_GAME=tl2` = ядро DOOM + `DM_GAME_D3` (общий SDK 8.2 / NVN 53) + `DM_GAME_TL2` (выключает D3-специфичное).

## Вход
| Файл | Что |
|---|---|
| `Torchlight 2\Torchlight II [010090400D366000][v0].nsp` (1.7 ГБ) | база |
| `...[010090400D366800][v327680].nsp` (0.5 ГБ) | апдейт v5 (BKTR) |
| `DLC\Torchlight II [DLC Yapper][010090400D367001][v0].nsp` | питомец (AOC 1), пока не подключён |

## Разведка (06.11.2026)
- Движок **Ogre 1.x + FMOD Ex**, порт Panic Button (как DOOM 2016 Switch). Видео — `PanicMoviePlayer` через `nn::movie` (MP4/H.264).
- Модули: `main` (build-id a0c6c2d2…), `sdk` **3ae34e68… = тот же SDK 8.2.0, что у D3**, `subsdk0` = NintendoSDK movie (Android Stagefright).
- main: 846 импортов, `nvnBootstrapLoader` единственный NVN-импорт (остальное через GetProcAddress), набор имён NVN = D3.
- npdm: prio 44, core 0, **SystemResourceSize 0** (без VAMM, как DOOM 2016), стек 1 МиБ. ProgramId 010090400D366000.
- RomFS (база + BKTR, IVFC ок): 257 файлов, 1.88 ГиБ: `PAKS/DATA*.PAK(+.MAN)`, `PROGRAMS/*.NVN` (шейдеры), `MOVIES/*.MP4`,
  `MUSIC`, `CONSOLELOC/<LANG>.LANGUAGE` (de en es fr it ja pl pt_br ru zh_cn zh_tw — все языки в RomFS), `SPLASH.XTX`.
  Пути в коде — `ROM:/PROGRAMS/…` (верхний регистр, как в RomFS).
- Глубина: сразу после инициализации устройства `nvnDeviceSetWindowOriginMode(1)` + `nvnDeviceSetDepthMode(1 = NEAR_IS_ZERO)`
  безусловно (main 0x199e4f8 / 0x199e50c; указатели процедур main+0x7702940 / +0x7702948) — как D3.

## Патч NSO
`DM_TARGET=android python src/dm/tools/patch_nso.py work/exefs-update/<mod> work/patched <mod>` для main, sdk, subsdk0;
`equiv.py`: **0 расхождений**, мутации 300/300 на всех трёх (main 6 550 мест, sdk 11 277 / 93 гейта, subsdk0 10 639).
- Новый вид таблицы переходов (main 0x19e0a6c): `adrp x18 + add x18; mov x0,x18; ldrsw x18,[x18,x13,lsl #2]; add x18,x18,x0; br x18`
  — загрузка по оригиналу базы, а `add` по копии. `jump_offsets` теперь принимает это, только если инструкция прямо перед загрузкой —
  `mov rb,<база загрузки>` (раньше на таких местах был assert, так что остальные сайты не меняются).

## Шейдеры
Каждый `PROGRAMS/*.NVN` (и `skin*_c.nvn`) — одна стадия, голый GLSLCoutput (магия 0x19866891, как `.sb` DOOM 2016 без 8-байтной
обёртки id): секции 0 GPU code, 1 asm, 2 perf, 3 reflection, 4 hash. `tools/tl2_shader_jobs.py` → `work/shaders-jobs` (формат
d3_shader_jobs.py, ключ nxnative control+code), пары vs/ps по именам из `CONSOLE_SHADERS.PROGRAM`, факт `depth_zero` для всех vs.
64 стадии (28 vs, 32 ps, 4 cs-скиннинг), 36 программ, 0 проблем. Перевод `dmsh_vk_xc2.py` (транслятор D3): **64/64 ok**, 4.98 МБ.
```
python src/tools/tl2_shader_jobs.py work/romfs/PROGRAMS work/shaders-jobs
XLATE=…/dmsh_xlate.exe SPIRV_OPT=… SPIRV_VAL=… J=12 python src/dm/gfx/tools/dmsh_vk_xc2.py work/shaders-jobs work/shaders-vk
```

## Гейты DM_GAME_TL2 (относительно D3)
| Где | TL2 |
|---|---|
| hle_svc.c SystemResourceSize | 0 (VAMM выкл); ALIAS_BIG/резерв как у D3 (безвредно) |
| hle_svc.c ProgramId | 0x010090400D366000 |
| main.c `dm_d3_wide_install`, hle_hid.c `dm_d3_ui_pump`, hle_d3_ui.c, JNI `nativeD3UiTap` | выкл (адреса main D3) |
| hle_args.c argv[0] | "tl2" |
| hle_fs.c хлебные крошки архивов | `.pak` |
| hle_misc.c AOC | как DOOM: нет, пока `DM_AOC_OWNED=1` (Yapper = индекс 1) |
| hle_oe.c язык | `lang_to_switch` без проверки CPK (все языки в RomFS) |
| hle_movie.c | Extractor отказывает в `SetDataSource(path)` (слот 2) → плеер (main 0x1a3e468) просто не играет ролик; объект от `operator new`, удаление — no-op; `SetCoreMask`, `MediaData::Clear` — no-op |
| hle_tl2.c (новый) | `audio::OpenAudioOut(out,name,param)` / `OpenDefaultAudioOut(out,param)` без события → hle_audio.c; `hid::GetTouchScreenState<16>` → touch_fill; `account::GetUserExistence` = true |
| Java | `D3 = d3 ‖ tl2` (фронт и раскладка D3), `TL2`: папка `Download/Torchlight2Mobile`, имя «Torchlight II Mobile», пакет `org.torchlight2.port` |

Статическая сверка импортов (main минус HLE минус D3): из нативных вызовов sdk остались сокеты / ldn / nifm::GetCurrentIpConfigInfo
(нужны только для сетевой игры), чистые библиотечные (os-синхронизация, MessageQueue, MultiWait, crypto, StandardAllocator,
audio-ресемплеры, JpegDecoder, DecompressZlib). Без HLE (заглушки) — 10 функций `movie::Buffer`: только в пути декодирования,
до которого при отказе источника не доходит.

## Сборка и запуск
```
cmake -S dm/android -B work/build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release -DDM_GAME=tl2
GRADLE=<gradle.bat> powershell -File tools/build_apk.ps1            # -> work/TL2-port-debug.apk (org.torchlight2.port)
bash tools/push_phone.sh                                             # dm/out, dm/shaders, turnip27, APK, RomFS -> Download/Torchlight2Mobile
bash tools/run.sh <имя> [сек] [ENV=…]                                # лог work/logs/<имя>.log, игра закрывается после
```
Данные на телефоне: `/sdcard/Download/Torchlight2Mobile/{romfs, dm/out, dm/shaders, dm/env, drivers, logs}`.

## Дальше
1. Первый запуск на Mi 14 Ultra (Turnip 27): HLE-дыры, первый present, меню, FPS.
2. Видео: MP4/H.264 через Android MediaCodec (сейчас ролики пропускаются).
3. Тач: игра импортирует `GetTouchScreenState` — проверить, принимает ли меню тач (фронт D3 сейчас шлёт только пад).
4. DLC Yapper (AOC 1), иконка, своя раскладка.
