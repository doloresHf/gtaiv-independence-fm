# Independence FM: Stream Apple Music (or Anything You Want) to Your Car Radio in GTA IV

Listen to audio from Apple Music or a compatible Windows app on Independence FM.
This is an unofficial mod. [中文安装指南](QUICKSTART-zh-CN.md) · [English quick start](QUICKSTART.md)

## Download

Open [Releases](https://github.com/doloresHf/gtaiv-independence-fm/releases) and choose the **win64 ZIP** under Assets.
Extract the entire ZIP first, then double-click **Setup.exe**.

Target configuration: Windows 11 x64, Steam GTA IV Complete Edition 1.2.0.59, a Win32 ASI loader, and two active audio outputs. Apple Music is a preset. Custom sources use Windows process-loopback capture with a compatible shared audio format. Browser tabs in the same process tree can be heard together. Exclusive, ASIO and spatial modes are outside the supported configuration. The beta's verified app and device combinations are listed in its release notes.

## Build

Use Windows with Visual Studio 2022 C++ Build Tools, a Windows SDK with process-loopback headers, CMake 3.22+, Python 3 and the Windows Media components.
From a clean checkout, run:

```powershell
powershell -NoProfile -File .\build-release.ps1
```

Use `-CMake <path>` if CMake is outside PATH. This builds Win32 and x64, runs offline regressions, generates and verifies the silent carriers, and puts the player ZIP and SHA-256 under `build/release/`. The installer and daily launcher do not require the build tools.

MIT for project code. The archive also includes the MinHook/HDE notices.
