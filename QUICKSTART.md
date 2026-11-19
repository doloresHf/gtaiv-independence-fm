# Independence FM — Quick Start (`v0.3.0-beta.1`)

## 1. Install or Upgrade
1. Launch GTA IV normally once, then exit it. Extract the full ZIP to a writable folder.
2. Run **Setup.exe**:
   - **Fresh install**: Select **GTAIV.exe** (Steam → Manage → Browse local files) and confirm your **User Music** folder. If the Win32 ASI loader (`dinput8.dll`) is missing, follow the Setup prompt to install the official x86 build. (If another mod already uses `dinput8.dll`, check its compatibility notes first.)
   - **Upgrading from `v0.2.0-beta.1`**: Make sure GTA IV and the tray app are closed, then run the new **Setup.exe** and choose **Yes** to upgrade in place while keeping your folders.

## 2. Configure Isolation & Audio Source
- **Zero-Hardware Auto Isolation (Recommended)**:
  - Open **Options → Settings (Ctrl+,)** in the tray app and check **Zero-Hardware Silent Isolation**.
  - If **Steam Streaming Speakers** or **VB-Audio Virtual Cable** is already on your PC, the tray app reuses it automatically with no extra hardware—your Windows default headphones/speakers stay untouched, your player stays quiet outside the game during streaming, and audio automatically restores to your speakers when you stop or exit.
  - If no virtual sink is detected yet, click **Install / Import Sink** to browse for a local WHQL `SteamStreamingSpeakers` driver folder (verified by SHA-256 before install) or install **VB-Cable** from its official page. (Always keep your real speakers/headphones as the Windows Default Playback device.)
- **Manual Dual-Output Mode (Optional)**:
  - Prefer two physical outputs instead of a virtual sink? Uncheck auto-isolation in Settings and route the music app in **Settings → System → Sound → Volume mixer** to an active, physically quiet secondary output while leaving GTA IV on your main output.
- **Audio App Choice**:
  - **Apple Music**: Captured automatically via preset. Ensure **Settings → Playback → Dolby Atmos** is set to **Off**.
  - **Custom App (Stream Anything)**: In tray **Options → Settings**, pick a playing shared-mode app or browse for its `.exe`. (Browsers share an audio process tree across tabs.)

## 3. Scan & Play
1. Start the tray app, then launch GTA IV and run **Complete Scan for Music** in Audio settings.
2. Get into a vehicle and tune to **Independence FM**. **Waiting for game** means GTA IV has not pulled from the bridge yet. Keep both apps running during play.

After installation, the tray app lives at `%LOCALAPPDATA%\GTAIV-Independence-FM\iv_apple_music_radio.exe`. You can delete the extracted ZIP folder once installed, or keep **Setup.exe** if you later want to repair or uninstall cleanly.

## Troubleshooting & Recovery
- **App still silent on desktop after a crash or forced close**: Open tray **Options → Settings** and click **Restore App Audio** (or run `iv_apple_music_radio.exe --restore-routes`), or start the bridge once and stop cleanly.
- **Station missing in game**: Verify all three `IV_Radio_Stream_01/02/03.mp3` carriers exist locally inside the active User Music folder (Wait for OneDrive sync if applicable), then run **Complete Scan for Music**.
- **Existing User Music tracks mix in**: Temporarily move personal tracks out of `User Music` and rescan in GTA IV; move them back and rescan whenever desired.
- **Persistent silence or double playback**: Confirm your music app is **unmuted** in Windows, Dolby Atmos (Apple Music) is **Off**, no exclusive ASIO / WASAPI Exclusive mode is enabled, and the virtual/secondary sink is **not** set as the Windows Default output.
- **Delays & skipping tracks**: Initial tune-in requires ~5.3 s of handshake silence, the game audio buffer delays live source switches by a few seconds, and carrier rollovers (~20 min) can insert a brief gap. Skip tracks in your player/media keys instead of pressing `B` / `N` in-game.
- **Uninstall**: Close GTA IV and exit the tray app, then run **Setup.exe → Uninstall**. Persisted app audio routes are automatically restored before removal; re-scan music in GTA IV afterward.
