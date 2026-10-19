# Grabbed by the Ghoulies — native Windows preview

An unofficial, experimental Windows runtime for the original Xbox game. Original game code runs natively; graphics use an NV2A OpenGL renderer and audio/input use Windows backends. This project is not affiliated with or endorsed by Rare or Microsoft.

**You must supply your own game files. No game, BIOS, saves, textures, music or other game assets are included.** Do not upload them to this repository or its issues.

## Getting started

1. Extract the complete preview package into a writable folder. Keep its directory structure intact.
2. Open **Ghoulies.exe** (or **Play Ghoulies.cmd**).
3. Browse to your own supported Xbox game-partition ISO.
4. Choose graphics options and select **Save settings & play**.

The launcher reads the image without modifying it, verifies the executable, and caches only the executable locally under `native-storage/launcher-cache`. It does not download game files. Compressed archives, full-disc images with a leading video partition, and other game revisions are not supported by this preview. Use an XDVDFS game-partition image with its volume descriptor at sector 32.

Supported executable: USA release, SHA-256
`a2a5bbe51ecc11392612bad30ee4238b796d4d0ed7262e7d23dc4120388294b1`.
This identifies one tested revision; it does not claim support for all regional releases.

## Graphics options

| Option | Choices |
|---|---|
| Render resolution | 1× (640×480), 2× (1280×960), **3× (1920×1440, recommended for most PCs)**, 4× (2560×1920) |
| Framerate | **Locked 30fps (default)**; 60fps (experimental) |
| Anisotropic filtering | **Original (default)**; 2×, 4×, 8×, 16× |

30fps uses the original two-field timing. Experimental 60fps uses the conserved-field timing patch and can slow down in heavy scenes; movement-startup and broader gameplay timing still need more testing. Resolution changes rendering size, not simulation speed. Select a lower resolution if GPU performance is insufficient. The display preserves the game's aspect ratio.

Anisotropic filtering can sharpen textures viewed at an angle, especially floors. It cannot create new texture detail. This preview enhances eligible mipmapped 2D textures with linear filtering; screen copies, non-mipmapped textures and nearest-filtered textures retain their original sampling. The renderer limits the setting to the GPU's supported maximum and records the effective value in the log. Quality and performance vary by GPU; Original is the compatibility fallback. Restart the game to apply changes.

## Requirements and controls

- 64-bit Windows with .NET Framework 4.8 and a graphics driver supporting the runtime's OpenGL 4.5 entry points. Windows on ARM, Linux/Proton and Steam Deck are unverified.
- The native game runtime is 32-bit. The launcher is 64-bit. No Visual Studio installation is needed to play a binary package.
- Tested hardware: Ryzen 5 7600X, RTX 5070 Ti, NVIDIA driver 610.88. This is a tested configuration, not a measured minimum requirement. Other GPU vendors remain unverified.
- An XInput-compatible controller is recommended; Bluetooth Xbox controllers have been tested. **F11** or **Alt+Enter** toggles fullscreen; **Esc** leaves fullscreen (and closes the game when already windowed).
- Keyboard overlay: WASD moves; arrow keys control the right stick; Enter is Start, Tab is Back, Space is A, Backspace is B, E/Q are X/Y, V/R are black/white; T/G/F/H are D-pad up/down/left/right, Z/C are triggers. Keyboard input requires the game window to have focus.

Settings are stored in `launcher-settings.json`. Saves and caches are in `native-storage/` alongside the launcher; keep this folder when upgrading. Normal play uses `native-storage/native-run`. The developer `--test-profile` option uses `test-run` instead; it does not import or merge saves. Do not run two game instances against the same saves.

## Known limitations and bug reports

See [known issues](KNOWN_ISSUES.md). This is a preview, not a claim of complete campaign compatibility or a guaranteed 60fps experience. Some unimplemented operations deliberately stop with an explicit report.

Each launch creates a timestamped `logs/` folder containing text diagnostics. The public launcher disables frame and audio-file capture. Use **Open logs** and attach `report.txt` and the selected settings when reporting an issue. Include the preview version/hash, CPU, GPU, driver, Windows version, scene and reproduction steps. Logs record the game image's file name and paths relative to the launcher folder, not your account name or folders. Do not attach disc images, executable caches, saves or memory dumps.

## Building from source

Use Visual Studio 2022 or newer Build Tools with **Desktop development with C++**, an x86 compiler and Windows SDK, plus .NET Framework 4.8. The scripts discover the installed C++ toolchain via `vswhere`.

From the source root, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_launcher.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_native.ps1
```

Open the resulting `Ghoulies.exe`. Building does not require or bundle game files. Running does. The package's `source/` directory contains the corresponding source for its launcher and runtime; build from that directory to reproduce the layout. Binary hashes may vary with compiler versions and build paths.

## Licence and attribution

Project-authored code is distributed under **GPL-2.0-only**; see [LICENSE](LICENSE). Third-party files retain their original copyright and licensing notices, including GPL/LGPL alternatives and permissive notices. See [THIRD_PARTY.md](THIRD_PARTY.md) and the bundled licence texts. The licence covers the runtime/tooling, not the original game.
