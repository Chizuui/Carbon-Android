<p align="center">
  <img src="https://raw.githubusercontent.com/{{REPO}}/main/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" width="96" alt="NFS Carbon">
</p>

<h1 align="center">NFS Carbon for Android {{VERSION}}</h1>

<p align="center">
  An unofficial native Android port of the original Xbox version of <b>Need for Speed: Carbon</b>,<br>
  statically recompiled with <a href="https://github.com/TGP482/XboxXBE-Recomp">XboxXBE-Recomp</a> and drawn with Vulkan.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Android-10%2B-3DDC84?logo=android&logoColor=white" alt="Android 10+">
  <img src="https://img.shields.io/badge/arch-arm64--v8a-blue" alt="arm64-v8a">
  <img src="https://img.shields.io/badge/Vulkan-1.1-AC162C?logo=vulkan&logoColor=white" alt="Vulkan 1.1">
  <img src="https://img.shields.io/badge/SDL-3-1D4F91" alt="SDL3">
</p>

> [!IMPORTANT]
> This release contains **no game assets**. You need the files from your own legally acquired copy of *Need for Speed: Carbon* (Xbox, USA release).

## 📦 Download

| File | Size | SHA-256 |
| --- | --- | --- |
| [`{{APK}}`](https://github.com/{{REPO}}/releases/download/{{TAG}}/{{APK}}) | {{SIZE}} | `{{SHA256}}` |

## 🚀 Installation

1. Install **`{{APK}}`** on your phone.
2. Open **NFS Carbon** and allow **all files access** when it asks.
3. Copy the game into **`/storage/emulated/0/Carbon`**, or into a `Carbon` folder on your SD card: the disc image (`.iso`), or `default.xbe` with the disc's `NFS` folder beside it.
4. The game starts as soon as it finds the files.

> [!TIP]
> If you don't grant all files access, put the game in `Android/data/com.racas.carbon/files/assets` instead. Settings and saves are stored in `Android/data/com.racas.carbon/files`; logs go in `Carbon/logs` (in `Android/data/com.racas.carbon/files/logs` without all files access).

> [!TIP]
> Updating from an older build installs right over it — your saves and settings are kept. The app also checks for new releases itself: open the settings panel and tap **CHECK FOR UPDATES**.

## ✨ Features

| | |
| --- | --- |
| 🖥️ **Graphics** | Render resolution 20–200%, LOW / MID / HIGH presets, brightness 50–200%, bloom, motion blur, light flares, road reflections, shadows, FXAA / SSAA 2x, sharpen, vivid, scanline and CRT filters, unlocked frame rate, V-Sync, FPS counter with latency (GOverlay style, vertical or horizontal), anisotropic filtering, custom GPU drivers (Turnip / AdrenoTools with auto-fallback) |
| 👆 **Touch Controls** | A full Xbox pad on screen, a layout editor to move and resize each control, stick mode and tilt steering |
| 🎮 **Other Input** | SDL3 controllers with rumble and rebindable controls (with a live input test), stick deadzone, keyboard with rebindable controls |
| ⚙️ **General** | Language (with Indonesian), audio output, update check, zip log export (with share sheet), pause while in the background, AI difficulty (`DEFAULT`–`EXTREME`), cheats (`UNLOCK ALL`, `FREEZE OPPONENTS`, `NO POLICE`, `UNLIMITED NOS`, `UNLIMITED SPEEDBREAKER`, `MAX CASH`) |

The settings panel opens from the gear at the top of the screen. Android's back button is the controller's Start.

## 📱 Requirements

- **Android** 10 or newer, arm64
- **GPU** with a Vulkan 1.1 driver (Adreno 6xx+, Mali-G7x+, Xclipse)
- **4 GB** RAM, **4 GB** free storage

## 🙏 Credits

Built on [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by **TGP482** and the Android host of [Black-Android](https://github.com/RanggaCasper/Black-Android). Forked from [RanggaCasper/MostWanted-Android](https://github.com/RanggaCasper/MostWanted-Android). Uses [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).

**Chizui: the *Need for Speed: Carbon* port.** Bug reports go to the [PotatoRecomp Discord](https://discord.gg/4DTA7nVfh7).

*Need for Speed* is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by them.
