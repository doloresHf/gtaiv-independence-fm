# Changelog

All notable changes to this project are documented in this file.

## [v0.3.0-beta.1] - Unreleased

### Added
- **Zero-Hardware Automatic Silent Isolation**: Eliminates the hard requirement for a second physical audio output device. Automatically detects existing virtual silent sinks (`Steam Streaming Speakers`, `VB-Audio Virtual Cable`, etc.), persists an atomic route journal, routes the isolated source app via `Windows.Media.Internal.AudioPolicyConfig` while streaming, and automatically restores it back to the user's original/system-default output when stopping, switching sources, or exiting (plus crash recovery on next start).
- **Virtual Silent Sink Settings & Recovery Controls**: Added *Zero-Hardware Silent Isolation* section in Options → Settings with auto-route toggle, live sink/hazard status check, local WHQL `SteamStreamingSpeakers` driver folder selector (strictly verified against pinned SHA-256 checksums before elevated SetupAPI installation with system-default endpoint protection), uninstall helper, and one-click *Restore App Audio* button (`--restore-routes`, `--install-virtual-sink`, `--uninstall-virtual-sink`, `--no-auto-route`).
- **`Setup.exe` In-Place Upgrade (`v1` → `v2` receipt)**: Added seamless in-place upgrade from `v0.2.0-beta.1` while keeping user-selected game/User Music directories and modified-file safety checks. Uninstall now restores persisted per-app audio routes cleanly and optionally offers to remove installed virtual speaker drivers.

### Changed
- Updated `build-release.ps1` with parameterized versioning, Route A compliance checks (zero third-party proprietary `.sys`/`.cat`/`.inf` binaries in source or ZIP), PE machine validation, and `candidate-manifest.json` generation.
- Updated `README.md`, `QUICKSTART.md`, and `QUICKSTART-zh-CN.md` to document single-output automatic isolation alongside optional manual split-endpoint routing.

## [v0.2.0-beta.1] - 2026-11-18
- Initial public preview release featuring bilingual tray launcher (`iv_apple_music_radio.exe`), *Stream Anything* source switcher, x86 `IVRadioProbe.asi` WMF hook, silent 20-minute carriers, and `Setup.exe` receipt-based installer/uninstaller.
