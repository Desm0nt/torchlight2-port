# TL2: ASTC-кэш текстур для GPU без BC (Mali, PowerVR, Xclipse)

KB: TEX-15 (пак D3), TEX-DOS2-01 (ленивый кодировщик), TEX-04 (нулевые блоки). Ветка `tl2-astc`, 16.11.2026.

## Зачем
Без BC (нет `textureCompressionBC`) движок распаковывает каждую BC-текстуру при заливке: ×8 памяти для BC1, ×4 для BC2/BC3 и декод
на каждой заливке. Лог игрока (Poco X6 Pro, Mali-G615, issue #4): `NVN BC decode vram=284 MB`, а в BC было бы 64.6 MB. С кэшем
текстура создаётся сразу в ASTC и заливается готовыми блоками с диска (`pread`), память ≈ размер BC, декода нет.

## Что в RomFS
`PAKS/DATA*.PAK`: 11 605 текстур (XTX), BC 2D 5863 (BC1 0x42: 3283, BC2 0x43: 2274, BC3 0x44: 306), **5622 уникальных, 815 МБ**;
RGBA8 0x25: 5696 (в кэш не входят), кубмапы BC: 38 (не входят, как у D3). Карт нормалей нет (ни одного `*_N` / `NORMAL`; `_SPEC` /
`_GLOW` — обычные RGBA-данные), поэтому отдельная раскладка каналов не нужна: кодируются и меряются все 4 канала как есть.

## Ключ (главный риск — снят офлайн)
Ключ D3: CRC-32 + Adler-32 байтов хранения текстуры в памяти игры, их число, NVN-формат, ширина, высота, уровни (`xbg_astc_cache.h`).
- Загрузчик игры `main+0x1995f40` (дизассемблировано): строит NVN-текстуру из инфо-блока XTX (ширина, высота, глубина, формат —
  0x41 подменяется на 0x42 —, уровни, target; без packaged layout и флагов), **сравнивает `nvnTextureBuilderGetStorageSize` с
  размером из XTX и при несовпадении текстуру не грузит**, затем `memcpy` блока данных XTX (тип 3) в свой пул и `SetStorage`. Значит
  хранение, которое видит движок при создании образа, — байт в байт блок данных XTX.
- `dm/android/tests/tl2_astc_key_test.cpp` (нативный читатель PAK + раскладка NVN движка `nvn_tex_layout`): для **всех 5863** BC 2D
  текстур размер по раскладке движка = размер из XTX = размер блока данных, автоматическая высота блока = высоте из XTX, 0 расхождений.
- Ключи C-теста и ПК-инструмента совпали для всех 5863 строк; `astc_cache_test` находит записи пака по дампу хранения.
- Только телефон покажет: что движок действительно видит те же байты в момент `get_image_slow` (никто не пишет в пул между `memcpy` и
  первым использованием). Признак — строки `NVN astc cache: miss (other bytes)` при полном паке: их должно быть ~0 (кроме модов).

## Три источника, один индекс
`dm/gfx/vulkan/engine/xbg_astc_cache_tl2.inc` (включён в `xbg_game.cpp` на месте путей создания текстур; `XbgAcEntry::pad[0]` = источник):
1. **Пак setup** `Download/Torchlight2Mobile/astc/tl2-astc.pack` (`D3AC` v1, только чтение): `tools/tl2_astc_cache.py`.
2. **Сборка на телефоне** `astc/tl2-astc.pack.part` + `.journal-phone` — пока не закончена (готовая становится `tl2-astc.pack`):
   `dm/android/astc_convert.cpp` (сборщик D3 с читателем PAK `dm/android/tl2_pak.h`), UI `Tl2Textures.java`: диалог при старте
   («Подготовить / Позже / Больше не спрашивать»), строка меню паузы «Подготовить текстуры» (после «Моды…»), прогресс, отмена,
   троттлинг по температуре, игра на паузе. Только на GPU без BC (`VkDrivers.kgsl()` = нет, или `DM_GFX_SIM_GPU` в dm/env).
3. **Ленивый кэш** `Android/data/org.torchlight2.port/files/astc/tl2-astc-lazy.pack.part` + `.journal-lazy`: промахи (моды, неполный
   пак, пака нет совсем) после первой заливки уходят фоновым потокам (`xbg_astc_lazy.cpp` из DOS2, nice 10, очередь с ограничением —
   кадр никогда не ждёт), кодируются по тому же правилу и дописываются; со следующего кадра попадают в индекс, живые распакованные
   образы пересоздаются из кэша при следующем использовании, следующие запуски берут сразу ASTC.

Правило качества (одно на все три пути, D3, хозяин 13.11): mip 0 ≥ 42 дБ PSNR (RGBA) против распакованного BC; BC1 6x6 → 5x5 → 4x4
fast → 4x4 medium → thorough; BC2/BC3 4x4 fast → medium → thorough; не дотянула — остаётся BC. Нулевые блоки → void extent (TEX-04),
движок проверяет ещё раз при заливке.

## Переключатели (игрокам ничего ставить не нужно)
| Переключатель | Умолчание | Что |
|---|---|---|
| `DM_GFX_ASTC_CACHE` | auto | auto = вкл., если устройство не сэмплирует BC (`textureCompressionBC=0`; `DM_GFX_SIM_GPU=mali` / `nobc` моделируют) и есть ASTC LDR; 1 = и рядом с BC (тест на Adreno); 0 = выкл. **Adreno с BC: auto = выкл, всё как было.** |
| `DM_ASTC_DATA_DIR` | ставит приложение: `Download/Torchlight2Mobile` | где папка `astc` |
| `DM_ASTC_CACHE_PATH` | — | другой файл пака setup |
| `DM_TEX_ASTC_CACHE_DIR` | ставит приложение: `Android/data/<пакет>/files/astc` | ленивый кэш |
| `DM_TEX_ASTC_PRODUCE` | 1 | 0 = без ленивого кодировщика |
| `DM_TEX_ASTC_THREADS` | 1 с паком, 2 без | потоки кодировщика (nice 10) |
| `DM_TEX_ASTC_CACHE_MB` / `DM_TEX_ASTC_QUEUE_MB` | 2048 / 64 | потолок ленивого пака / очереди копий |

## Строки boot.log
- `NVN astc cache: using N textures: pack … (… MB, путь), phone build …, lazy … ; encoder on|off …; blocks 4x4=1 5x5=1 6x6=1; device BC=0 native_bc=…` — один раз.
- `NVN astc cache: not used (<причина>); BC textures are decoded` — например `the device samples BC` на Adreno.
- `NVN astc cache: <pack> not used: <why>` — пак есть, но битый / неполный.
- `NVN astc cache: miss (no texture of this shape) …` / `miss (other bytes) … crc= adler=` — первые 12 промахов с причиной.
- `NVN astc cache: upload serial=… fmt=0x42 … -> ASTC 6x6 … from pack (mip 0 … dB)` — первые 8 заливок.
- `NVN astc cache: lookups 64/128/256…: hits … (pack, phone, lazy), misses … (not in the cache, other bytes, block size …) … live X MB of ASTC where decoded would be Y MB (saved Z MB) …`.
- `NVN PERF astccache hits= misses= … live= decoded_would_be= saved= lazy_cached=` — в блоке NVN PERF.
- `NVN astc lazy: serial=… -> ASTC 4x4 (mip 0 … dB …)`, `NVN astc lazy: N textures done …` — ленивый кодировщик.
- `[astc] start / … packs, … textures to read / … unique BC textures … / done: …` — сборка на телефоне.
- `NVN BC decode vram=…` — должно расти намного меньше прежних 284 МБ.

## Setup (ПК)
`python -I tools/tl2_astc_cache.py <work>/Torchlight2Mobile/romfs <work>/Torchlight2Mobile/astc [--jobs N]` (зависимости —
`tools/requirements-astc.txt`). Шаг setup «8/8 Preparing textures for phones without BC» (`setup_tl2.py`: шаг `astc` по умолчанию,
`--skip-astc`, `check`, `push` копирует `astc/`, если пак готов; `bootstrap.ps1` / `setup.sh`: пакеты `astc-encoder-py==0.1.12`,
`numpy<2.4`).
Пак не обязателен: без него телефон соберёт его сам (меню паузы) или наберёт лениво.

## Замеры на ПК (16.11, 12 потоков, этот RomFS)
- `tl2_astc_cache.py`: 5622 уникальных → **5595 в паке, 27 остаются BC** (19 BC2, 8 BC3: ниже 42 дБ и на 4x4 thorough), пак
  **800 МБ** (838 868 864 Б) при BC-хранении 813 МБ этих текстур, **67 с** (план 7 с), нулевых блоков 0. Блоки: BC1 6x6 1924, 5x5 909,
  4x4 297; BC2 4x4 2189; BC3 4x4 276. `--verify`: индекс, диапазоны, 0 нулевых, выборка 64 шт. ≥ 42.4 дБ.
- Сборщик телефона на ПК (`astc_convert_host`, 6 потоков SSE4.1): те же 5595 записей и блоки, 5510 байт в байт с паком ПК (85 — ±0.01 дБ,
  разные SIMD), **50 с**, 1.13 Гпикс. Оценка для телефона в диалоге: `astc_conv_estimate_minutes()` (4 больших ядра ~3 ГГц ≈ 5 мин).
- Ленивый кодировщик (`astc_lazy_test`) на 6 текстурах TL2: 6 закэшировано, журнал читается парсером движка, PSNR совпадает.

## Малый пак для теста на телефоне
Полный пак 800 МБ — по правилу «без больших пушей» не заливать на телефон по медленному adb без спроса. Малый: интерфейс (виден сразу в меню),
70 текстур, **28 МБ**:
```
python -I tools/tl2_astc_cache.py <romfs> <out>/astc --only "media/ui/*"
adb push <out>/astc/tl2-astc.pack <out>/astc/manifest.json /sdcard/Download/Torchlight2Mobile/astc/
```
Прогон: Mi 14 Ultra, `DM_GFX_SIM_GPU=mali` в dm/env (Adreno без BC в модели) → в boot.log `using … pack 70`, `upload … from pack`,
`hits` в меню; остальное — `miss (no texture of this shape)` и `NVN astc lazy: …` (ленивый кэш набирает в фоне); второй запуск —
`lazy N` в строке `using`. Без SIM на Adreno — `not used (the device samples BC …)`. Другой вариант без ПК: «Подготовить текстуры» на
телефоне (~5 мин, пак ~0.8 ГБ в Download). Сравнить `NVN BC decode vram=` и `saved` с прежними 284 МБ.

## Проверено / не проверено
- Проверено офлайн: ключ (выше), пак ПК и пак телефона совпадают, ленивый кодировщик на данных TL2, APK собирается.
- **Не проверено на устройстве:** попадания в реальной игре (строки `miss (other bytes)`), реальный Mali (декодер ASTC, sRGB у TL2 нет —
  форматы 0x42–0x44 UNORM), время сборки на телефоне, влияние ленивого кодировщика на кадр при входе в новую локацию (он на nice 10 и
  с очередью, но 1–2 потока CPU у слабых телефонов заметны), UI диалога/строки меню (не открывался).
- Риски: место +0.8 ГБ (Download) и до 2 ГБ ленивого кэша (данные приложения); моды, меняющие текстуры, кэшируются лениво; если игра
  когда-нибудь пишет в хранение текстуры после первого использования — `changed later` → образ пересоздаётся на BC-декодере (как в D3).
