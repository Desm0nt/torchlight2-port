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

## Широкий экран (DM_TL2_WIDE, по умолчанию вкл., 07.11.2026; на телефоне ещё не проверено)
Размер TL2 у системы не спрашивает: константы рядом с `oe::GetOperationMode` (main = `main.raw`, build a0c6c2d2):
| Где | Что |
|---|---|
| `0x199417c orr w27,#1920` / `0x199418c mov w27,#1280` (+ `mov w8,#1080/#720`) | окно NVN: 3 swap-текстуры (SetSize2D `0x1994430`), размер render window `+0x24/+0x28` (`0x1994954`), цель `0x1991260`; перестраивается по флагу `+0x5f4` |
| `0x7264b4 orr w8,#1920` / `0x7264cc mov w20,#1280` | init игры: ПК-настройки WIDTH/HEIGHT (id `0x753f4c4/c8`, дефолт 800x600 в `0x270ad0`) |
| `0x1317b8 orr w2,#1920` / `0x131720 mov w21,#1280` | то же по сообщению о смене режима (цикл TryPopNotificationMessage) |
| `0x72cce0` CreateViewports | аспект камер = actual width / height вьюпорта → `setAspectRatio`, FOV вертикальный ⇒ шире окно = **Hor+** |
| `0x72e330` → `0x275a90` | применение видеонастроек: снап WIDTH/HEIGHT к ближайшему режиму из списка, затем аспект камер из них — снап сделан тождеством |
| `0x140f28 orr w3,#1920` → `0x19fef80` | холст консольного UI (CConsoleUI*, «UI_CONSOLE») 1920x1080 в обоих режимах (масштабируется на окно); полноэкранные панели — литерал (1920,1080) `0x1f71810`/`0x1f69298` |

`dm/host/hle_tl2_wide.c` (`dm_tl2_wide_install`, из `main.c` после `load_modules`): сверка 6 окон слов, затем окно/настройки = W x 1080
(портатив W·2/3 x 720), W = 1080 × аспект экрана (кратно 16; 3200x1440 → 2400). Холст UI по умолчанию остаётся 1920x1080 = HUD
растянут ×W/1920, как было при растянутой 16:9-картинке. Ручки: `DM_TL2_WIDE=0` (как на Switch) / `=<ширина>`; `DM_TL2_WIDE_UI=1` —
расширить и холст UI (эксперимент). Java: шестерёнка «Формат картинки», для TL2 по умолчанию «широкий» → `DM_TL2_WIDE=1|0`.
**Экраны загрузки — 16:9 с чёрными полосами** (по умолчанию; `DM_TL2_WIDE_LOADSTRETCH=1` / шестерёнка «растягивать экраны загрузки» —
растянуть). Экран загрузки — узел консольного UI `CConsoleUILoading` (vtable `0x2304530`, создаётся `0xc1e40` из `0x141020`, указатель в
менеджере UI `+0x90`; менеджер — статик `main+0x74cfe70`, GOT `0x23a3590`). Байт узла `+0x74` = рисуется (базовый Render `0x19f57b0`
без него выходит), `+0x75` = обновляется: Show `0xc2a30` ставит оба (`0xc2ae4 strh #0x101`), Update `0xc30f0` гасит по концу
затухания (`0xc3168`); игра сама проверяет его в `0x112718`. Пока `+0x74` ≠ 0, презентер (`vulkan_present.cpp`, хук
`dm_present_pillarbox`, поток трансляции) показывает всё окно W x 1080 в 16:9 по центру, по бокам чёрное (холст 1920x1080 сжимается
обратно в свои пропорции, линейная фильтрация). С `DM_TL2_WIDE_UI=1` не включается.
Тач: игра читает панель только раз при старте (`0x13ea04`), менять нечего. В nxnative/nx2android общего механизма нет (у них
пер-игровые патчи `DM_SCREEN_ASPECT`).

## Сборка и запуск
```
cmake -S dm/android -B work/build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release -DDM_GAME=tl2
GRADLE=<gradle.bat> powershell -File tools/build_apk.ps1            # -> work/TL2-port-debug.apk (org.torchlight2.port)
bash tools/push_phone.sh                                             # dm/out, dm/shaders, turnip27, APK, RomFS -> Download/Torchlight2Mobile
bash tools/run.sh <имя> [сек] [ENV=…]                                # лог work/logs/<имя>.log, игра закрывается после
```
Данные на телефоне: `/sdcard/Download/Torchlight2Mobile/{romfs, dm/out, dm/shaders, dm/env, drivers, logs}`.

## Первые запуски (07.11.2026, Mi 14 Ultra, Turnip 27)
- r2: первый кадр рисуется `nvnCommandBufferDrawTexture` (у нас не было) → «present a texture never rendered». Сделано: запись
  `OP_X_DRAW_TEX` (hle_nvn.c) и `draw_texture` в xbg_game.cpp — хэндл из текущего пула, blit с фильтрацией и конвертацией формата
  в colour target 0 (`draw_tex_blit` разрешает разные форматы). Блендинг и отражение регионов не учитываются (сплэш/меню непрозрачные).
- r4: `nn::fs::IsSaveDataExisting(ApplicationId, Uid)` (main+0x1335e4) шёл в нативный fsp-srv — хук был только в hle_xc2.c.
  Перенесены в hle_tl2.c: этот хук, `MountSaveDataReadOnly`, `ec::ShowShopApplicationInformation`, плюс по сверке импортов TL2 с
  nxnative/nx2android (MIT): ldn после Initialize, `nifm::GetCurrentIpConfigInfo`, `socket::GAIStrError` / `SendTo(MsgFlag)` /
  `Shutdown(ShutdownMethod)`, геттеры AudioIn, `account::AsyncContext::operator=`. os/util/mem/crypto из сверки не нужны: это код
  SDK без сервисов, их не перехватывают и nxnative/nx2android. Сверка «по именам» должна учитывать все строки `"_Z…"` (макросы
  SKF/SKD/SSF в hle_net.c) и гейты (хуки из hle_xc2.c в сборку tl2 не входят; `oe::Enter/LeaveExitRequestHandlingSection` — в hle_d3.c).
- r6–r7: меню и создание персонажа, 60 fps (gpu ~12 мс, submit ~6 мс).
- Скиннинг: TL2 скинит на GPU compute-шейдерами `skin1..4_c` (палитра костей — UBO 0 = c3, 64 байта на кость, `nVerts` в c3+0x4000;
  s0 вход, s1 веса, s2 выход). Симптом: у всех персонажей, NPC и монстров нет анимаций, bind-поза, шипы из рук и рогов. Игра
  анимирует нормально (хэш палитры меняется каждый бинд). Причина: `tl2_shader_jobs.py` не писал факты compute `local` / `shared`
  (control +0x748 / +0x754), транслятор объявлял LocalSize 1,1,1, а шейдер считает индекс как CTAID.X * 64 + TID.X — скинилась
  одна вершина из 64. Исправлено в tl2_shader_jobs.py (как xc2_shader_jobs.py), перевод заново: изменились только 4 cs.
- Отладка по Wi-Fi adb: `work/bin/radb` (adb с переподключением, RADB_HOST=ip:port; ADB=…/radb для run.sh / shots.sh) и
  `work/bin/shot` (скриншот, бинарно-безопасный). run.sh в конце закрывает игру — не запускать, пока на телефоне играют.

## Дальше
1. Широкий экран 20:9 Hor+ и тач по D2R Mobile (в работе отдельно).
2. Видео: MP4/H.264 через Android MediaCodec (сейчас ролики пропускаются).
3. Тач: игра импортирует `GetTouchScreenState` — проверить, принимает ли меню тач (фронт D3 сейчас шлёт только пад).
4. DLC Yapper (AOC 1), иконка, своя раскладка.

## Тач (07.11.2026)
**Статика: у Switch-TL2 тача нет.** `nn::hid::GetTouchScreenState<16>` вызывается ровно один раз — в init hid (main 0x13e9d0:
InitializeNpad → InitializeTouchScreen → SetSupportedNpadStyleSet(7) → SetSupportedNpadIdType → `GetTouchScreenState(this+0x278)` 0x13ea04),
опрос пада (0x13ea60, GetNpadStyleSet / GetNpadState ×3 стиля) тач не читает; других вызовов (bl / b на PLT 0x1df97c0) нет. Мыши/клавиатуры в
импортах нет. CEGUI 0.6.2 внутри, но имена событий мыши вырезаны (нет строк `MouseClick` / `MouseButtonDown` / `Clicked`), cvar
`input_forceGamepad` регистрируется (0x6b940c) и больше нигде не читается. Меню — консольные фокусные / радиальные
(`CConsoleUIMenu`, `CConsoleUIButtonMenu`, `CConsoleUIRadialMenu`, `CConsoleUIMainMenu`, `CConsoleUIStartMenu`, `CMenuController`).
Итого тач в игру слать некуда (hle_tl2.c отдаёт touch_fill честно, но читается он один раз при старте); весь тач — во фронтенде → npad.
Тап по конкретному пункту меню — см. «Тап по пунктам меню» ниже (DM_TL2_POINTER, за гейтом).

**Раскладка `tl2` (по D2R Mobile, `Tl2Touch.java`; D3PadView только передаёт ей касания/рисование, по умолчанию для TL2):** плавающий стик
(левые 40% × нижние 55%, от точки касания, 60 dp полный ход, 6 dp мёртвая зона, поправка диагонали `tl2_iso`), веер справа как у D2R
(A основной, B X Y R, ZR на месте портала, II = второй набор: модификатор `tl2_mod` за 170 мс до навыка), перетаскивание навыка = прицел
(`tl2_aim` правый / левый стик / выкл), короткий тап = импульс с направлением, увод пальца на «−» = отмена; нижний ряд D2R-«пояса» = ←↑↓→ +,
сверху ZL L L3 / − R3 / ⚙ / МЕНЮ. Мир: тап = `tl2_tap` (выкл / A / повернуться+A), держать = идти к пальцу (`tl2_hold_walk`, якорь героя
`tl2_hero_y`). Режим меню (как панельный режим D2R): тап = A, долгое = держать A, два пальца = B, свайп = шаги крестовины (`tl2_nav_swipe` 0)
или левый стик (1, радиальные меню); игра стартует в нём, + / − включают его сами (`tl2_auto_nav`). Иконки навыков D3 (D3SkillIcons) для TL2 выключены.

**Пересмотр после проверки владельцем (07.11.2026):** режима меню больше нет (тап = A / свайп = крестовина / два пальца = B / МЕНЮ-ИГРА /
авто по + −) — убран из кода и ⚙ до настоящего тача по пунктам (мышиные события CEGUI, отдельная ветка). По умолчанию (`tl2`): два
плавающих стика по D2R — ход (левый стик, левые 40 % × нижние 55 %) и прицел (правый стик, свободное место правее 50 % ширины и ниже 35 %
высоты; кнопки важнее), оба 60 dp / 6 dp с поправкой `tl2_iso`; веер A B X Y R ZR + II, ряд ←↑↓→ +, верх ZL L L3 / − R3 / ⚙ — простые кнопки.
Выключено по умолчанию: `tl2_tap` (тап по миру), `tl2_hold_walk`, `tl2_aim` (прицел перетаскиванием навыка). Вторая раскладка
`tl2_side` (по выбору): ячейки быстрого доступа по краям — справа A B X Y R ZR и рядом «мод+»A…ZR (модификатор `tl2_mod` раньше на 170 мс),
слева L ZL ↑←→↓, сверху L3 − ⚙ + R3; те же два стика. Привязки консольной TL2 статически не найдены (MEDIA/KEYMAPPING/*.DAT — PC-клавиши),
поэтому ячейки подписаны кнопками Switch, а модификатор второго набора выбирается в ⚙.
## Тап по пунктам меню (DM_TL2_POINTER, 07.11.2026; на телефоне ещё не проверено, по умолчанию выкл.)
Мыши в Switch-TL2 нет и не будет: CEGUI 0.6.2 слинкован, но консольные меню — свой UI Panic Button **PUI** (RTTI `N3PUI7ElementE`,
`4List`, `6Button`, `6Radial`, `7Manager`, `9UserDrawn`, `10ScrollPane`), а фокус у каждого `CConsoleUI*`-меню свой (главное меню:
индекс `+0x5c` над массивом кнопок `+0x98`, `0x1457e0`; другие двигают выделение `PUI::List`). Писать индекс в игру нельзя (у меню своя
копия), поэтому тап = **замкнутый цикл через пад**: найти пункт под пальцем → прочитать, что сейчас в фокусе → шаг крестовиной в его
сторону (держать 50 мс, отпустить 70 мс) → перечитать фокус → … → A только когда игра сама показывает целевой пункт в фокусе. Шаг, который
фокус не сдвинул, пробует другую ось; не сдвинулось ничего — конец без A. Лимиты 24 шага / 3 с.
| Что | Где (main.raw) |
|---|---|
| `PUI::Manager` | статик `0x927a970` (GOT `0x23a3678`), init `0x19fef80` из `0x140f00`: `+0x2f8/+0x2fc` холст (1920x1080), `+0x308` корень; Render `0x19f5090` — корень дважды, проход 0 и 1 |
| `PUI::Element` vt `0x23651f8` | `+0x38` родитель, `+0x40` первый ребёнок, `+0x58` следующий (рисуются по порядку = позже выше), `+0x60` l,t,r,b в единицах холста (Layout vt+0x70 `0x19f5a90`), `+0x74` рисуется (Render `0x19f57b0` без него не рисует ни его, ни детей), `+0x80` слой (1 = второй проход, над всем) |
| `PUI::List` vt `0x23656d8` | Draw `0x1a01ab0`: `+0x84` l,t,r,b области строк, `+0x100` высота строки (int), `+0x104` строк, `+0x108` выбранная (SetSelected `0x1a02460`), `+0x10c` прокрутка, `+0x116` показывать выбор; строка i: y = t + i·h − scroll |
| `PUI::Button` vt `0x2365838` | `+0xc4` подсвечена (SetHighlighted `0x1a05020`), `+0xc3` серая (`0x1a05060`) |
| `PUI::Radial` vt `0x2365628` | радиальные меню — стиком; тап по ним только в лог |
| Меню-стек | менеджер консольного UI `0x74cfe70`: `+0x58` массив, `+0x60` верх; HandleInput `0x143800` раздаёт событие сверху вниз |
| Опрос пада | ввод игры `0x13fd40` → `0x13ea60` (главный поток), события с автоповтором `0x13eea0` |

`dm/host/hle_tl2_ui.c`: `dm_tl2_ui_tap(x, y)` (доли картинки, любой поток) ставит тап в очередь, `dm_tl2_ui_pump` (hle_hid.c fill, поток
ввода игры) ведёт шаги; JNI `DmActivity.nativeTl2UiTap(float, float)` → false, если гейт выкл. Кнопка без подсвеченной кнопки в своей
группе (общий предок не выше 4 уровней — подписи, подсказки) не цель. Ручки: `DM_TL2_POINTER=1` / `=2` (мимо пунктов = A, как старый
тап меню); `DM_TL2_UIDUMP=1|2` + файл `dm/ui-dump-now` → `dm/tl2-ui-dump.tsv` (дерево PUI); файл `dm/ui-tap-now` с «x y» = тап без фронта.
Все места сверяются по словам (+ GOT и vtable), другая сборка → выкл.
Не покрыто: элементы `UserDrawn` (сетка инвентаря, дерево навыков и т. п. — если они такие), радиальные меню, прокрутка списка пальцем.
Java (не подключено, Tl2Touch.java правится параллельно): `static native boolean nativeTl2UiTap(float x, float y);` в DmActivity; в
Tl2Touch, режим меню, короткий тап: вместо `pulse(K_A, 0, 0, 0, false)` —
`if (!DmActivity.nativeTl2UiTap(f.x0 / view.getWidth(), f.y0 / view.getHeight())) pulse(K_A, 0, 0, 0, false);` (картинка TL2 = весь view).
