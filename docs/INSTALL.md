# Installing Torchlight II Mobile — step by step

Русская версия: [INSTALL.ru.md](INSTALL.ru.md).

Torchlight II Mobile runs **your own copy** of Torchlight II for Nintendo Switch on an Android phone. This project
contains no game files and never shares game files or console keys: you dump them from a Switch you own. A setup program
on your PC turns your dump into a game folder (~2 GB), you copy that folder to the phone and install the app.

The whole thing takes about 15 minutes, most of it copying.

## What you need

**From your Switch** (see step 1):

| What | Details |
|---|---|
| Torchlight II base game | title id `010090400D366000`, from your cartridge or eShop copy |
| Torchlight II update **v327680** | title id `010090400D366800` (release 5, the latest). Only this exact update works (the port is made for its code). Dump tools usually put it in the file name, e.g. `...[010090400D366800][v327680].nsp` |
| `prod.keys` | the keys of **your own** console |

No language pack is needed: every language of the game (English, German, French, Spanish, Italian, Polish, Portuguese
(Brazil), Russian, Japanese, Chinese) is in the base game. The Yapper pet DLC is not used yet.

The game and the update can be **NSP, NSZ, XCI or XCZ**, in any combination. Split dumps from a FAT32 SD card work too
(a folder with parts `00`, `01`, … or `name.xc0`, `name.xc1`, …). File names do not matter: setup recognises the files
by their content.

**A PC** with Windows 10/11 (64-bit), Linux or macOS:

- about **4 GB of free disk space** while preparing (up to ~6 GB with NSZ / XCZ input);
- internet on the first run: setup downloads a few helper tools (Python packages ~40 MB, Microsoft .NET ~300 MB, and
  Python itself ~15 MB on Windows if you have none). It asks before each download and puts them only inside its own
  folder. Later runs work offline.

**A phone:**

- Android 12 or newer, 64-bit (Android 10 and 11: the separate `-android10` APK, Step 6);
- **Snapdragon with an Adreno GPU recommended** (tested on Snapdragon 8 Gen 3; Mali / PowerVR / Xclipse phones use their
  own driver with engine fallbacks and are untested);
- 8 GB of RAM or more;
- about **3 GB free** in internal storage;
- a USB cable (or another way to copy the folder to the phone).

## Step 1. Get the game files and keys from your Switch

**Dumping them yourself.** This needs a Switch that can run homebrew (how to get there is not covered here).
1. Install the game and the latest update (v327680) on the Switch.
2. Dump them with [nxdumptool](https://github.com/DarkMatterCore/nxdumptool): installed or eShop copy → *NSP*;
   cartridge → *XCI* (the installed update is dumped separately as NSP). For NSP dumps turn on
   **"remove console specific data"** — otherwise the update cannot be read on a PC.
3. Dump the console keys with [Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM). You get `prod.keys` on the SD card
   in `switch/`.
4. Copy the files to your PC.

Do not download game files or keys from the internet, and do not share yours.

## Step 2. Get Torchlight II Mobile setup

Download this repository as a ZIP (green **Code** button → **Download ZIP**) or clone it, and extract it to a disk with
enough free space, into a short path such as `D:\TL2-setup`. You get a folder with `setup.bat`, `setup.sh` and a folder
`put-game-files-here`.

## Step 3. Put your files into `put-game-files-here`

```
<setup folder>/
├── setup.bat
├── setup.sh
└── put-game-files-here/
    ├── Torchlight II ... [010090400D366000] ... .nsp              (or .nsz / .xci / .xcz — the base game)
    ├── Torchlight II ... [010090400D366800][v327680] ... .nsp     (the update)
    └── prod.keys
```

You may also put the folder from your dump tool inside it as it is (setup looks two folders deep).

## Step 4. Run setup

### Windows

1. **Double-click `setup.bat`.** If Windows warns about a file from the internet, choose **Run** (or **More info → Run anyway**).
2. Answer the questions with **Y** and Enter (Enter alone also means yes): *Download a private Python?* (only when your PC
   has no Python 3.10+), *Install the Python packages?*, *Download and install the .NET 10 SDK?* Everything goes into
   `tools\.runtime` inside the setup folder. Nothing is installed system-wide, no admin rights are needed.
3. **Wait.** Setup shows seven numbered stages with progress:

   | Stage | Usually takes |
   |---|---|
   | 1. Finding and checking your game files | seconds |
   | 2. Checking the tools and the free space | seconds (first time: the .NET download) |
   | 3. Extracting the game program | seconds |
   | 4. Patching the program for Android | under a minute |
   | 5. Checking the patch | 1–5 min |
   | 6. Extracting the game data (~1.9 GB) | 1–5 min |
   | 7. Translating the shaders | 1–3 min |

   It checks the free space before it starts and stops early with a clear message when something is wrong. If you
   close the window or the PC turns off, just run setup again: finished stages are skipped.
4. At the end it shows where the finished folder is, the adb commands to copy it, and what to do next.

Instead of using `put-game-files-here` you can also drag the files onto `setup.bat`.

### Linux / macOS

1. You need Python 3.10 or newer with its `venv` module (Debian / Ubuntu: `sudo apt install python3 python3-venv python3-pip`;
   Fedora: `sudo dnf install python3 python3-pip`; Arch: `sudo pacman -S python python-pip`; macOS: `brew install python@3.12`).
2. Open a terminal in the setup folder and run `bash setup.sh`. Answer the questions with **Y** and wait (same stages
   as on Windows). Python packages go into `tools/.runtime/venv`, .NET into `tools/.runtime/dotnet`.

You can also give the files directly, in any order: `bash setup.sh ~/dumps/tl2-base.nsp ~/dumps/tl2-update.nsp ~/prod.keys`.

### Not sure what is missing?

Run `setup.bat check` (Windows, in a terminal in the setup folder) or `bash setup.sh check`. It lists the tools, the
game files it finds, what is already prepared and the next step, and changes nothing.

## Step 5. Copy the game folder to the phone

The finished folder is **`work/Torchlight2Mobile`** inside the setup folder.

- **USB file transfer:** connect the phone, choose **File transfer**, open the phone's **internal storage** →
  **Download** and copy the whole **`Torchlight2Mobile`** folder there. It must end up exactly like this:

  ```
  Internal storage/Download/Torchlight2Mobile/romfs/...
  Internal storage/Download/Torchlight2Mobile/dm/out/...
  Internal storage/Download/Torchlight2Mobile/dm/shaders/...
  Internal storage/Download/Torchlight2Mobile/mods/          (your PC mods go here, see below)
  ```

- **adb (USB debugging):** `setup.bat push` / `bash setup.sh push` copies only what is missing and resumes after an
  interruption; setup also prints the plain `adb push` commands.

## Step 6. Install the app

Install the APK from the [latest release](https://github.com/Desm0nt/torchlight2-port/releases/latest), or build it
yourself (`build.bat`, see *Building* in the [README](../README.md)). Android asks you to allow installing apps from
your browser or file manager the first time.

- Android 12 or newer: `Torchlight2-Mobile-v<version>-arm64.apk` (`TL2-port-debug.apk` when you build it yourself).
- Android 10 or 11: `Torchlight2-Mobile-v<version>-arm64-android10.apk` (the same game built for older Android; it asks
  for the storage permission instead of *All files access*; not yet tested on a real Android 10 device).

## Step 7. First launch

1. Start **Torchlight II Mobile**.
2. It asks for **All files access** (it needs it to read `Download/Torchlight2Mobile`). Turn it on and go back.
3. The first start and the first loads take longer than later ones. The gear button opens the pause menu and the port's
   settings (language, picture shape / widescreen, docked / handheld, frame rate limit, driver, controls, mods).

**Playing by touch:** the move stick appears under your left thumb anywhere in the lower left; skills are in the fan on
the right (II switches to the second skill set); the thin slider on the right edge zooms the camera; tap the minimap
for the big map. Menus work by tapping their items directly; lists and long texts scroll with a swipe. *Game settings…
→ Edit the layout…* lets you move, resize and hide every control; the side layout puts every button in its own cell
along the screen edges.

Saves and settings stay in the app's own storage. Installing a newer APK over the old one keeps them; the pause menu can
export and import saves.

## PC mods (optional)

1. Copy Torchlight II PC mods (`.MOD` files, as made with GUTS) into `Download/Torchlight2Mobile/mods/` on the phone.
2. In the game open the gear button → *Game settings…* → **Mods (on: N)…**, tick the mods you want and order them with
   the arrows (a mod lower in the list wins where two change the same file), then *Save*.
3. Close the game and start it again. The app converts the ticked mods once (a progress window shows for big mods).
   While any mod is on, the game uses a separate save folder (a copy of your saves made the first time); your normal
   saves are not changed, and untick all mods to get back to them.

Mod data, models, textures and sounds are used. Changes to the PC game's interface are not (the Switch version has its
own console interface), so mods that need new interface screens or DLL plugins will not work fully.

## Troubleshooting / FAQ

**"your Torchlight II update is v… ; only v327680 is supported"** — dump update v327680 (release 5).

**"the Torchlight II base game / the update … is not among the files"** — setup lists every file it found with its title
id and version; check that you copied the right files into `put-game-files-here`.

**"prod.keys not found" / "your prod.keys has no …" / "does not decrypt with this prod.keys"** — use the keys of the
console you dumped from, made with an up-to-date Lockpick_RCM. `title.keys` is a different file.

**"personalized ticket" / "no ticket for rights id"** — dump the NSP again with nxdumptool's *remove console specific
data* option turned on (and keep the ticket).

**"not enough free space"** — free some space, or put the work folder on another disk: `setup.bat --work D:\tl2-work`
(Windows) or `bash setup.sh --work /mnt/big/tl2-work`.

**Where is the language setting?** — gear button → *Game settings…* → *Game language*; restart the game afterwards.
All languages are built in.

**The intro video does not play** — videos are skipped in this version; the game simply continues.

**A mod does nothing / the game looks wrong with a mod** — untick it in *Mods* and restart. Mods that replace the PC
interface or add classes without console assets are not supported. `boot.log` lists `[tl2 mods]` lines with what was
converted.

**Black screen or the app closes** — open the gear dialog → *Vulkan driver* → try *System*, restart the app. Then open an
issue on this repository with the phone model, Android version and what happened, and attach the whole
`Download/Torchlight2Mobile/logs/` folder: after a crash the next start writes `crash.txt` there (with the log of the
crashed run) and says so on screen; `boot.log` and `logcat.txt` of the current run and `boot.prev.log` /
`logcat.prev.txt` of the one before are written line by line while the game runs (so they are there even when Android
kills the game). Every setup run is logged to `work/setup.log`: attach it for setup problems.

Technical details of the preparation (what each step does, running the tools by hand): [tl2/GAME_FILES.md](tl2/GAME_FILES.md).
