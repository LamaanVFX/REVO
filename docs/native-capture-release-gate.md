# Native capture release gate: full legacy feature parity

This is the acceptance contract for replacing Klip's OBS/libobs capture engine. The native WGC/D3D11/FFmpeg experiment is **not a release replacement** until every applicable item below is implemented and verified. Do not remove or silently downgrade an existing setting to make the native build easier.

## Required user experience

- [ ] Keep every existing setting and UI option, including its saved value and runtime effect.
- [ ] Settings apply immediately when previously supported, persist across restarts, and accurately indicate when a restart is required.
- [ ] Keep replay buffer, save-clip behavior, configurable clip duration, rolling-buffer sizing, normal recording, output folders, naming, container selection, encoder/quality, FPS, bitrate, scaling/resolution, cursor/border, overlays, capture preview, source selection, microphone selection, and all hotkeys.
- [ ] Preserve all existing configuration keys and migrate old settings without resetting them.
- [ ] No UI control may be decorative, ignored, or silently unsupported.

## Capture

- [ ] Native Windows Graphics Capture + D3D11 texture path; do not use the OBS game-capture hook.
- [ ] Keep frames GPU-resident for conversion and hardware encoding wherever supported; avoid CPU readback and unnecessary texture copies/synchronization.
- [ ] Capture must continue when Klip is focused, minimized, restored, or tabbed into; handle game focus changes and capture-target closure/recreation without freezing.
- [ ] Enumerate sources, detect games automatically, switch targets, and update the game profile automatically.
- [ ] Preserve the prior game profiles: Minecraft/javaw.exe uses 240 FPS and 35,000 kbps; other games use 60 FPS and 20,000 kbps.
- [ ] Prefer NVIDIA NVENC on supported NVIDIA GPUs; keep existing supported encoder selection/fallback semantics.

## Audio and output

- [ ] Keep desktop/system audio, microphone, game-process audio, Discord, and Spotify/music capture and their existing individual mixer controls.
- [ ] Preserve the five-track output behavior when separate tracks are enabled; each track must contain the intended source, not an empty or duplicated mix.
- [ ] Preserve audio application discovery, persistent executable-based selections, independent gain controls, device/app recovery, synchronization, and track routing.
- [ ] Verify real output files, not just successful compilation: MP4 and MKV where supported, replay clips and regular recordings, all expected streams, correct durations, and audible source-specific track contents.

## Release acceptance tests

- [ ] Focus/tab into Klip repeatedly while a real game is captured; verify no freeze and no stalled frame timestamps.
- [ ] Launch Minecraft and another game; verify automatic detection, profile changes, and target recovery.
- [ ] Verify game, desktop, Discord, Spotify/music, and microphone audio independently.
- [ ] Verify five-track recordings, replay clips, normal recordings, hotkeys, source switching, settings persistence, and live settings application.
- [ ] Package every required runtime dependency and verify the packaged app starts on a clean Windows environment.
- [ ] Run repeated real-game A/B captures with PresentMon and compare p95/p99 frame times as well as average FPS. No performance claim based only on synthetic tests or CI.
- [ ] Keep the OBS build available as a reference until native parity is demonstrated.

## Performance claim

The target is **ShadowPlay-like low-overhead capture** using a native Windows/D3D11/GPU-encoding pipeline. This does not mean copying or claiming to use NVIDIA's private ShadowPlay implementation. Publish no performance-equivalence claim without repeatable real-game measurements.
