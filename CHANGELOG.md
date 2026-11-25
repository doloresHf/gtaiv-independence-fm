# Changelog

All notable changes to this project are documented in this file.

## [v0.4.1-beta.1] - 2026-11-25

### Added
- **Independence FM Radio Mode Sync (`IVRadioBridge.asi` + WinRT SMTC)**:
  - Added x86 engine adapter `IVRadioBridge.asi` (`src/route_b_hooks.*`, `src/route_b_offsets.*`, `src/route_b_ipc_consumer.*`, `src/route_b_asi_main.cpp`) with 8 ASLR-safe masked prologue signature fuses for Steam GTA IV Complete Edition `1.2.0.59` and mutual-exclusion check against `IVRadioProbe.asi`.
  - Added x64 WinRT SMTC session controller (`src/route_b_smtc.*` bound to `AppleInc.AppleMusicWin`), deterministic coordinator (`src/route_b_coordinator.*`), and 512-byte cross-bitness control mapping `Local\GTAIV_RadioBridge_Ctrl_v1` (`src/route_b_ctrl_protocol.h`).
  - Synchronizes track endings with GTA IV DJ / commercials / Weazel News breaks without interrupting native audio, pre-rolls incoming songs cleanly at `0:00`, supports seamless back-to-back song handoffs (`SetSongEndBarrier` + gap-aware `FillPreloadedBlocksAtPlay`), and preserves unread tail audio in RAM (`PreserveUnreadTailIfApproachingLap`) so multi-minute `ESC` pauses resume without skipping ahead.
  - Supports in-game `N` (next track) and `B` (Apple Music previous/restart) while playing songs while ignoring `B`/`N` during DJ/ad breaks and passing `B`/`N` through natively in Sequential mode or when Radio Mode sync is inactive/disabled.
  - Added bilingual Settings checkbox, Row 5 live Radio Mode status display, `rb_*` session telemetry fields, `--no-radio-bridge` CLI flag, and `release/IVRadioBridge.ini`.
- **Route B Offline Verification Suite**: Added Win32 and x64 `route_b_offline_selftest` (Suites 1–5 including long-pause tail preservation, song-end barrier, gap-aware preload, and coordinator state machine coverage) to `CMakeLists.txt` and `build-release.ps1`.

### Changed
- **Upgraded `Setup.exe` Receipt (`v1`/`v2` → `IndependenceFM-v3`)**: Automatically retires `IVRadioProbe.{asi,ini}` and installs `IVRadioBridge.{asi,ini}` during transactional in-place upgrades from `v0.2.0-beta.1` and `v0.3.0-beta.1`, guards fresh installs against unmanaged `IVRadioProbe.asi`, and expands `Setup.exe --selftest`.
- Updated `README.md`, `QUICKSTART.md`, and `QUICKSTART-zh-CN.md` for `v0.4.1-beta.1` Radio Mode usage and upgrades.

### Fixed
- **Clean `0:00` Restart When Tuning In Mid-DJ**: Entering DJ/commercial/news breaks (`PausedForDj`) mid-song—such as tuning back into Independence FM partway through a track or encountering consecutive DJ segments—now resets Apple Music cleanly back to `0:00` via guarded `RequestPauseAndRestartTrack()` (using `TrySkipPreviousAsync()` past Apple Music's `Seek=0` SMTC profile when safe) and holds a `300 ms` settle window (`djPauseSettleMs`) before pre-rolling the incoming track.
- **ESC Single-Frame Glitch Debounce & SMTC Play/Pause Watchdogs**: Added a `100 ms` assertion debounce (`escPauseDebounceMs`) to filter GTA IV `17–21 ms` world/clock-transition pause glitches (while keeping `ESC` resume instantaneous at `0 ms`), plus bounded SMTC Play/Pause confirmation watchdogs (`TickSmtcVerify`) with ESC/DJ re-entry ownership transfer so Apple Music never remains stuck paused after leaving the pause menu (including off-station playback).

## [v0.3.0-beta.1] - 2026-11-19

### Added
- **Zero-Hardware Automatic Silent Isolation**: Eliminates the hard requirement for a second physical audio output device. Automatically detects existing virtual silent sinks (`Steam Streaming Speakers`, `VB-Audio Virtual Cable`, etc.), persists an atomic route journal, routes the isolated source app via `Windows.Media.Internal.AudioPolicyConfig` while streaming, and automatically restores it back to the user's original/system-default output when stopping, switching sources, or exiting (plus crash recovery on next start).
- **Virtual Silent Sink Settings & Recovery Controls**: Added *Zero-Hardware Silent Isolation* section in Options → Settings with auto-route toggle, live sink/hazard status check, local WHQL `SteamStreamingSpeakers` driver folder selector (strictly verified against pinned SHA-256 checksums before elevated SetupAPI installation with system-default endpoint protection), uninstall helper, and one-click *Restore App Audio* button (`--restore-routes`, `--install-virtual-sink`, `--uninstall-virtual-sink`, `--no-auto-route`).
- **`Setup.exe` In-Place Upgrade (`v1` → `v2` receipt)**: Added seamless in-place upgrade from `v0.2.0-beta.1` while keeping user-selected game/User Music directories and modified-file safety checks. Uninstall now restores persisted per-app audio routes cleanly and optionally offers to remove installed virtual speaker drivers.

### Changed
- Updated `build-release.ps1` with parameterized versioning, Route A compliance checks (zero third-party proprietary `.sys`/`.cat`/`.inf` binaries in source or ZIP), PE machine validation, and `candidate-manifest.json` generation.
- Updated `README.md`, `QUICKSTART.md`, and `QUICKSTART-zh-CN.md` to document single-output automatic isolation alongside optional manual split-endpoint routing.

## [v0.2.0-beta.1] - 2026-11-18
- Initial public preview release featuring bilingual tray launcher (`iv_apple_music_radio.exe`), *Stream Anything* source switcher, x86 `IVRadioProbe.asi` WMF hook, silent 20-minute carriers, and `Setup.exe` receipt-based installer/uninstaller.
