# Native capture experiment

This experiment builds the existing Windows Graphics Capture (WGC) + D3D11 + FFmpeg/NVENC engine without changing the shipping libobs build.

Run on Windows x64 from the repository root in PowerShell:

```powershell
.\tools\build-native.ps1 -Configuration Release
```

The script uses the repository's pinned validation toolchain, builds into a separate `build-native-Release-native-experiment` directory, and runs CTest. It does not overwrite the current libobs build.

## What this does and does not establish

- The native engine keeps captured textures on the D3D11/GPU path and can submit D3D11 textures to the hardware encoder; this is a candidate for reducing capture overhead.
- This does **not** use ShadowPlay's private capture implementation, and it does not by itself prove lower in-game frame-time loss.
- The shipping build remains libobs-based until the native build compiles and its replay, audio, recording, synchronization and real-game performance tests pass.
- Compare both builds on the same game/scene, resolution, output FPS, encoder quality, audio configuration and warm-up. Capture source-only / capture-on / source-only frame-time traces and compare p95/p99 frame times as well as average FPS. Repeat trials in alternating order.
- Do not use a synthetic capture scene as proof of real-game performance. Do not advertise a performance win without repeatable measurements.

Native capture is experimental. If the native build fails, preserve its full CMake/CTest output; do not switch the release build to it.