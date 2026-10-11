# Klip 2.0.1 beta — 2026-10-10

Desktop source commit: `262b747e4433ae2cede68a2048aac92d27df92bf`.
Release application SHA256:
`225d94cf4436376a274b155e1d6766055e7b67b7f180ee453c568fbfffc3fd97`.

Debug and Release passed all three deterministic suites. The actual Release
2.0.1 application passed `run-obs-test.ps1 -BuildSuffix release-201
-ReplayDurationTest -NoiseScene -GlobalHotkeys -Name release-201-duration-20261010`.
It saved three 1080p60 NVENC replays during recording, at decoded spans 30.467,
32.017 and 30.867 seconds, zero repeated source IDs and monotonic PTS. The
recording decoded at 60.0003 distinct updates/s with zero repeated IDs. Real
global input, tray/minimize and restore were exercised. Evidence is preserved in
`work/release-201-duration-20261010`. The extra GOP can make saves slightly longer
than the requested time, not an exact-frame trim.

The extracted portable archive passed every staged-file SHA256 check, application
version 2.0.1 verification and exact-byte equality with the capture-tested build.
The loader/settings probe exited 0; this probe alone is not capture verification.
The complete folders and dependencies are retained. Installer, portable and
corresponding-source hashes are in the published SHA256SUMS.txt.

The website's seven rendered/API/artifact tests, TypeScript check and source lint
passed. Three pre-existing img-element warnings remain. Generated `.vercel`
bundles are excluded from source lint, not application source. The download route
selects versioned 2.0.1 artifacts; existing aggregate counter keys are unchanged.

This is still unsigned public beta. No clean-machine installer certification or
all-GPU claim is made. AMD/Intel and Linux desktop capture were not verified in
this patch. Duration-first can reduce scene detail and also applies to recordings
sharing the encoder. Existing settings adopt it unless explicitly opting out;
the buffer needs warmup after startup or capture changes.
