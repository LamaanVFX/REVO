# Replay duration fix — 2026-10-10

## Cause and behavior

Klip 2.0 used hardware CQP / software CRF with a fixed packet-memory budget.
These modes have scene-dependent bitrate. OBS's replay output evicts complete
keyframe groups when either its time limit or its memory limit is reached.
Consequently, selecting 30 seconds did not imply that 30 seconds were retained.
Additionally, the duration selector recalculated memory using `video_bitrate`,
which was unused by the CQ encoder, and could shrink the replay budget.

Duration-first is now the default, including existing settings files without the
new fields. It uses CBR and fits the video bitrate to the explicit packet budget,
requested duration and all configured AAC tracks. It reserves 25% for bursts and
overhead, budgets four extra seconds, and retains an extra two-second GOP to avoid
discarding the first partial GOP. The memory cap is still enforced by OBS; it is
not disabled or automatically increased. Selecting a duration no longer resizes
the OBS memory budget using an unrelated legacy bitrate.

Settings expose the target and effective video bitrate. Constant quality remains
an explicit opt-out, with a warning that it can shorten clips. Duration-first
also affects concurrent recordings because they intentionally share one encoder.
It can reduce visual detail in complex scenes; this is not a claim of identical
quality to CQ18. Existing short files cannot be extended retroactively. A newly
started or reset buffer needs time to acquire history; the dashboard shows warmup.
Keyframe-aligned saves may run up to approximately two seconds over the requested
duration. Unsupported encoder settings fail clearly rather than silently falling
back to a different encoder/rate-control mode.

## Verification

Windows native OBS Game Capture of the controlled D3D11 scene, NVIDIA NVENC,
1920x1080 at 60 FPS, 192 MiB replay packet cap, two-second keyframes, Balanced P5.
Synthetic shader work=8 deliberately produces extremely difficult-to-compress
video; it is not a typical game workload or a performance benchmark. The CQ
baseline uses the retained constant-quality option on this branch, not a claim
of testing every previous shipped binary. The cases ran sequentially; analysis
of earlier files could overlap later capture. Do not attribute cadence differences
to the rate-control change as a causal FPS benchmark.

| Mode | Three clip durations | Decoded cadence | Repeated frame IDs |
| --- | --- | --- | --- |
| CQP18 / memory-limited | 2.067, 2.467, 2.917 s | 58.97–59.18 distinct/s | 0.58–0.82% |
| Duration-first / 35,336 kbps | 30.067, 30.467, 30.917 s | 59.93–59.97 distinct/s | 0% |

The fixed clips contain 1802, 1827 and 1853 decoded video frames with monotonic
presentation timestamps. All three were requested using real global hotkeys
while recording; the acceptance run also exercised hide/restore/minimize/tray.
Clip files were 132,847,292–136,474,227 bytes, below the 201,326,592-byte packet
budget. File size is not a measurement of total process RAM.
The concurrent fixed recording also fully decoded at 60.0003 distinct frames/s,
zero repeated source IDs and monotonic PTS. Both stress harnesses completed with
exit code 0; the fixed harness additionally enforced full decoded replay span.

An additional lighter shader-work=1 case with three AAC tracks and synchronized
audio/video markers produced 30.067–30.917-second clips at an effective 34,952
kbps. Median marker offset was +4 ms; each decoded clip passed the 40 ms sync
gate and had monotonic PTS. It had approximately 0.38% repeated source IDs; it is
not presented as perfect cadence. The initial work=1 CQP comparison retained the
full duration, hence the work=8 case was needed to reproduce memory exhaustion.

Debug and Release builds passed all three core/output-reservation/UI-state test
suites. Added regression coverage verifies bitrate/audio/memory calculations,
GOP rounding, rejecting impossible budgets, persistent settings, migration from
old settings and reconfiguration on policy changes.

Evidence directories: `work/replay-duration-cqp-stress-20261010`,
`work/replay-duration-fixed-stress-20261010`,
`work/replay-duration-after-20261010`. Logs, settings, real MKV files and decoded
analysis JSON are preserved locally, not uploaded.

## Reproduction commands (PowerShell, repository root)

```powershell
./tools/build-obs.ps1 -Configuration Debug -BuildSuffix replay-duration
./tools/build-obs.ps1 -Configuration Release -BuildSuffix replay-duration
./tools/run-obs-test.ps1 -BuildSuffix replay-duration -ReplayDurationTest -ConstantQuality -NoiseScene -Name duration-cqp-new
./tools/run-obs-test.ps1 -BuildSuffix replay-duration -ReplayDurationTest -NoiseScene -GlobalHotkeys -Name duration-fixed-new
./tools/run-obs-test.ps1 -BuildSuffix replay-duration -ReplayDurationTest -NoiseScene -NoiseWork 1 -SeparateAudioTracks -AvMarkers -Name duration-audio-new
```

Use fresh test names. The harness preserves prior evidence and checks decoded
video timestamp span, frame-ID cadence, audio tracks and optional A/V sync.
It checks all three fixed clips, not just container metadata. AMD and Intel CBR
property support was checked against pinned upstream source, but those hardware
paths were not exercised for this change. Test on other GPUs and real Fortnite
before making broader compatibility or game-performance claims.

Local test executable: `build-obs-Release-replay-duration/bin/64bit/Klip.exe`.
SHA256: `5e8f0eebb1744719d0154551d3a4f69b763d7abe59c2650a5bf7b48ce1fd0a19`.
Keep the complete build directory together. No installer, website or public
release was replaced by this fix.
