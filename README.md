# REVO 3.0.4

REVO is a Windows x64 game capture, replay clipping, and recording application built around stock OBS/libobs 32.1.2. It uses the supplied REVO cyan mark and a dark, cyan-accented main window.

## Current features

- Save recent gameplay clips and record full sessions using configurable global shortcuts. Three shortcut slots are available for both save clip and start/stop recording; single keys are supported where Windows allows them.
- Use NVIDIA NVENC H.264 for normal apps and games. When REVO detects Minecraft through `javaw.exe` or `minecraft.windows.exe`, it switches to NVIDIA NVENC HEVC/H.265; once Minecraft closes, it returns to H.264. HEVC is reported unavailable rather than silently claimed if the encoder cannot be loaded.
- Route game, microphone, Discord, and Spotify audio to their separate AAC tracks, alongside the combined desktop + microphone + Spotify track.
- Keep clips and recordings grouped by detected game with the matching filename prefix, including Minecraft, Fortnite, GTA, and Valorant.
- Show a translucent purple, always-on-top status toast on every connected monitor for 3 seconds when a clip is saved or recording starts/stops.
- Configure compatibility, video, capture, audio, shortcuts, and storage together in one scrollable Settings page in the main window. Settings are retained across upgrades.
- Select 30, 60, 120, or 240 FPS and a supported encoder. Unavailable encoder selections are reported rather than silently changing quality.
- Use explicit NVENC Performance tuning when its quality/compression tradeoff suits you. The optional game-copy limiter is not a guaranteed smoothness fix and does not cap the game's frame rate.
- Duration-first replay fits bitrate to the selected duration and memory budget. Clips and recordings default to MKV; MP4 is also selectable.

## Public beta

This is an unsigned public beta; compatibility and performance vary by game, GPU, and drivers. It does not claim universal OBS parity, zero overhead, or stable-release certification. A supported GPU/driver is required for hardware capture verification.

## Download and run

The repository, issues, source, and CI build artifacts are available at [github.com/LamaanVFX/klipedit](https://github.com/LamaanVFX/klipedit).

Install the complete `REVO-3.0.4-win64-setup.exe`, or extract the complete `REVO-3.0.4-win64-portable.zip` and open its root `REVO.exe`. Keep `bin`, `data`, `obs-plugins`, `licenses`, and `revo.ico` together. REVO requires Windows x64, supported graphics drivers, and the [Microsoft VC++ 2015–2022 x64 runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe). Verify the release SHA256SUMS; do not disable antivirus or Windows security.

The close button hides REVO; choose **Quit REVO** in the tray menu to stop capture and exit. Existing Klip settings are copied forward when REVO settings do not already exist. Existing media is left in place, and any saved custom folders remain selected rather than being moved or deleted.

Media, settings, and logs stay local. Review logs for private target/device names before sharing. See [Privacy](PRIVACY.md).

## Source, build, and verification

REVO is **GPL-3.0-or-later**; see [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md). The package includes corresponding upstream source/build archives alongside each beta build.

See [build/release instructions](docs/release-2.0.md), [architecture](docs/architecture.md), [gameplay performance evidence](docs/gameplay-performance-results.md), and [libobs verification](docs/libobs-results.md). The legacy custom WGC/FFmpeg engine remains in source behind `KLIP_USE_LIBOBS=OFF`; it is not the shipping engine. Distributed builds use libobs with diagnostic hooks off.

## Original project demonstration

https://x.com/andrew1x_/status/2079314121766396172

This earlier video demonstrates the original concept, not a benchmark or verification of the current REVO beta.
