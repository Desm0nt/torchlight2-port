Torchlight II Mobile - put your game files here
==============================================

Put these things into this folder (any file names are fine):

  1. Torchlight II base game     title 010090400D366000 v0         NSP, NSZ, XCI or XCZ
  2. Torchlight II update v5     title 010090400D366800 v327680    NSP, NSZ, XCI or XCZ
                                 (an XCI that already contains this update counts as both 1 and 2)
  3. prod.keys                   the keys of YOUR OWN console (dumped with Lockpick_RCM)

All game languages are already in the base game; no language pack is needed. The Yapper pet DLC is not used yet (setup
skips it if you put it here).

Split dumps from a FAT32 SD card work too: the folder with 00, 01, ... parts, or name.xc0 + name.xc1 ...

Then run setup:
  Windows:        double-click setup.bat (one folder up)
  Linux / macOS:  ./setup.sh  (one folder up)

It prepares work/Torchlight2Mobile with romfs, dm/out, dm/shaders and an empty mods folder (~2 GB); copy that folder to
the phone as Download/Torchlight2Mobile (setup prints the adb commands, or use: setup push).

Your files are only read; nothing is copied, changed or uploaded. Everything in this folder except this
README is ignored by Git. Dump the game and the keys from a Switch you own: they are never shared by this
project. Step-by-step guide: docs/INSTALL.md (Russian: docs/INSTALL.ru.md).

----------------------------------------------------------------------------------------------------------

Torchlight II Mobile - положите сюда файлы игры
===============================================

Положите в эту папку (имена файлов любые):

  1. Базовая игра Torchlight II   title 010090400D366000 v0         NSP, NSZ, XCI или XCZ
  2. Обновление v5                title 010090400D366800 v327680    NSP, NSZ, XCI или XCZ
                                  (XCI, в котором уже есть это обновление, считается сразу за 1 и 2)
  3. prod.keys                    ключи ВАШЕЙ консоли (снятые через Lockpick_RCM)

Все языки игры (в том числе русский) уже есть в базовой игре, языковые пакеты не нужны. DLC с питомцем Yapper пока не
используется (если положить его сюда, установка его пропустит).

Подходят и разбитые дампы с карты FAT32: папка с частями 00, 01, ... или name.xc0 + name.xc1 ...

Затем запустите установку:
  Windows:        двойной щелчок по setup.bat (на уровень выше)
  Linux / macOS:  ./setup.sh  (на уровень выше)

Получится папка work/Torchlight2Mobile (romfs, dm/out, dm/shaders и пустая папка mods, ~2 ГБ); скопируйте её на
телефон как Download/Torchlight2Mobile (установка напечатает команды adb, или: setup push).

Файлы только читаются: ничего не копируется, не меняется и не отправляется. Подробная инструкция:
docs/INSTALL.ru.md (по-английски: docs/INSTALL.md).
