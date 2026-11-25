# Independence FM: Stream Apple Music (or Anything You Want) to Your Car Radio in GTA IV

Listen to audio from Apple Music or a compatible Windows app on Independence FM in GTA IV without an extra hardware audio device or manual Volume Mixer toggling.
This is an unofficial mod. [中文安装指南](QUICKSTART-zh-CN.md) · [English quick start](QUICKSTART.md) · [Changelog](CHANGELOG.md)

## Download

Open [Releases](https://github.com/doloresHf/gtaiv-independence-fm/releases) and choose the **win64 ZIP** under Assets.
Extract the entire ZIP first, then double-click **Setup.exe** (also supports seamless in-place upgrades from `v0.2.0-beta.1` and `v0.3.0-beta.1` while keeping your installed paths).

## What is New in `v0.4.1-beta.1`

- **Native Independence FM Radio Mode Sync (`IVRadioBridge.asi` + WinRT SMTC)**:
  - **DJ, Commercials & Weazel News**: When GTA IV Audio Settings → **Independence FM Mode** is set to **Radio** with Apple Music selected, ending a track automatically cues native DJ/ads/news without interruption, pauses Apple Music, and pre-rolls the next song from `0:00` cleanly when the DJ finishes (including cleanly restarting mid-song playback to `0:00` if you tune in during a DJ break or across chained DJ segments, while keeping consecutive game-scheduled songs seamless).
  - **ESC Pause Synchronization (Multi-Minute & Glitch Safe)**: Opening the ESC pause menu pauses Apple Music in sync and snaps over pause silence on resume; consumer-side tail preservation prevents skipping ahead even after multi-minute pauses, while `100 ms` glitch debounce and SMTC Play/Pause confirmation watchdogs prevent Apple Music from staying stuck paused (including off-station playback).
  - **In-Game `N` / `B` Skip Hotkeys**: Pressing `N` (next) or `B` (Apple Music previous/restart) jumps tracks smoothly while a song is playing, while presses during DJ/ad/news breaks are ignored safely so native segments are never interrupted.
  - **Automatic Fallback**: Switching GTA IV to **Sequential** / **Shuffle** or picking a custom **Stream Anything** app automatically disables SMTC sync while keeping continuous streaming intact (`IVRadioBridge.asi` verifies 8 masked prologue signatures for GTA IV `1.2.0.59` x86 on load and aborts safely if mismatched or if `IVRadioProbe.asi` is also present).
- **`Setup.exe` `v3` In-Place Upgrade**: Automatically migrates `v0.2.0-beta.1` (`IndependenceFM-v1`) and `v0.3.0-beta.1` (`IndependenceFM-v2`) installations to `IVRadioBridge.{asi,ini}` (`IndependenceFM-v3`), retiring `IVRadioProbe.{asi,ini}` cleanly with rollback protection.

## Previously in `v0.3.0-beta.1`

- **Zero-Hardware Automatic Silent Isolation**: Single-headphone / single-speaker PCs are supported without needing a second physical DAC. When a compatible virtual silent sink (such as `Steam Streaming Speakers` or `VB-Audio Virtual Cable`) is present, the tray app automatically routes only your chosen audio app to the virtual silent sink while active, keeps Windows default playback/recording devices untouched for GTA IV, and automatically restores your music app back to your main speakers when stopping or exiting (including crash recovery).
- **Clean Public Packaging (No Third-Party Proprietary Binaries)**: To stay 100% compliant with MIT licensing, this repository and release ZIP do **not** redistribute proprietary third-party kernel drivers. Existing `Steam Streaming Speakers` / `VB-Cable` virtual sinks are detected automatically; if none is present yet, **Options → Settings** can detect/verify local Steam WHQL driver files (by pinned SHA-256), let you pick a local WHQL driver folder, or link to VB-Cable (and manual secondary-endpoint Volume Mixer routing remains supported).
- **In-Place Upgrade & Safe Uninstall**: `Setup.exe` upgrades existing `v0.2.0-beta.1` installations in place, restores persisted per-app audio routes before uninstalling, and preserves modified files and unrelated User Music tracks.

## Requirements & Compatibility

- Windows 11 x64 (`AudioPolicyConfig` + Windows process-loopback APIs), Steam GTA IV Complete Edition `1.2.0.59` (x86), and an x86 ASI loader (`dinput8.dll`).
- Apple Music is configured as a preset (**Settings → Playback → Dolby Atmos → Off**). Custom sources use Windows process-loopback capture with a compatible shared-mode audio format. Browser tabs in the same process tree can be heard together.
- Exclusive mode (WASAPI Exclusive / ASIO) and spatial audio modes are outside the supported configuration. The beta release notes list the verified app and device combinations.

## Build

Use Windows with Visual Studio 2022 C++ Build Tools, a Windows SDK with process-loopback headers (`10.0.22621+`), CMake 3.22+, Python 3, and Windows Media Foundation components.
From a clean checkout, run:

```powershell
powershell -NoProfile -File .\build-release.ps1
```

Use `-CMake <path>` if CMake is outside `PATH`. This builds Win32 (`IVRadioBridge.asi`, `IVRadioProbe.asi`) and x64 (`iv_apple_music_radio.exe`, `Setup.exe`), runs all offline regressions (`ipc_protocol_selftest`, `wmf_selftest`, `stage_d_*`, `stage_e_offline_selftest`, Win32/x64 `route_b_offline_selftest`, `Setup.exe --selftest`), generates and verifies the 20-minute silent MP3 carriers, and writes the player ZIP, `.sha256`, and `candidate-manifest.json` under `build/release/`. The installer and daily tray launcher require no build tools.

## License

MIT for project source code (`LICENSE`). See `THIRD-PARTY-NOTICES.txt` for MinHook / HDE licensing. No third-party proprietary kernel drivers are distributed in this repository.
