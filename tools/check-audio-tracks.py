#!/usr/bin/env python3
"""Verify five audio streams for desktop+mic, mic, game, Discord, and Spotify."""
import array
import json
import math
import pathlib
import subprocess
import sys

if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--streams-only"):
    raise SystemExit("usage: check-audio-tracks.py MEDIA_FILE FFMPEG_BIN [--streams-only]")
streams_only = len(sys.argv) == 4
media = pathlib.Path(sys.argv[1])
tools = pathlib.Path(sys.argv[2])
ffmpeg = str(tools / "ffmpeg.exe")
ffprobe = str(tools / "ffprobe.exe")

probe = json.loads(subprocess.check_output([
    ffprobe, "-v", "error", "-show_streams", "-of", "json", str(media)
], text=True))
audio_streams = [s for s in probe["streams"] if s.get("codec_type") == "audio"]
if len(audio_streams) != 5:
    raise SystemExit(f"expected 5 audio streams, found {len(audio_streams)}")
expected_titles = [
    "REVO desktop AAC", "REVO microphone AAC", "REVO game AAC",
    "REVO Discord AAC", "REVO Spotify AAC",
]
for index, (stream, expected_title) in enumerate(zip(audio_streams, expected_titles)):
    actual_title = stream.get("tags", {}).get("title", "")
    if actual_title != expected_title:
        raise SystemExit(
            f"audio stream {index} title mismatch: expected {expected_title!r}, "
            f"found {actual_title!r}"
        )

if streams_only:
    print(json.dumps({
        "file": str(media), "audio_streams": len(audio_streams),
        "signal_test": "skipped: Windows runner has no render endpoint",
        "titles": [s.get("tags", {}).get("title", "") for s in audio_streams],
    }, indent=2))
    raise SystemExit(0)

def tone_amplitude(stream_index, frequency):
    raw = subprocess.check_output([
        ffmpeg, "-v", "error", "-i", str(media), "-map", f"0:a:{stream_index}",
        "-ac", "1", "-ar", "48000", "-f", "f32le", "pipe:1"
    ])
    values = array.array("f")
    values.frombytes(raw[:len(raw) - len(raw) % values.itemsize])
    window = 4800  # 100 ms
    omega = 2.0 * math.pi * frequency / 48000.0
    amplitudes = []
    for start in range(0, len(values) - window + 1, window):
        block = values[start:start + window]
        real = sum(value * math.cos(omega * i) for i, value in enumerate(block))
        imag = sum(value * math.sin(omega * i) for i, value in enumerate(block))
        amplitudes.append(2.0 * math.hypot(real, imag) / window)
    if not amplitudes:
        return 0.0
    amplitudes.sort()
    return amplitudes[len(amplitudes) // 2]

# Stream order: desktop+mic, mic-only, game, Discord, Spotify.
# CI has no physical microphone input, so the mic-only track is not signal-tested.
required = {
    0: {440: 0.015, 660: 0.015, 880: 0.015},
    2: {440: 0.025},
    3: {660: 0.025},
    4: {880: 0.025},
}
measurements = {}
for stream, tones in required.items():
    for frequency, threshold in tones.items():
        amplitude = tone_amplitude(stream, frequency)
        measurements[f"stream_{stream}_{frequency}Hz"] = round(amplitude, 4)
        if amplitude < threshold:
            raise SystemExit(
                f"audio track {stream} missing {frequency} Hz test signal "
                f"(amplitude {amplitude:.4f}, expected >= {threshold:.3f})"
            )

# Dedicated stems must not contain one another's tone at a comparable level.
for stream, own_frequency in ((2, 440), (3, 660), (4, 880)):
    for other_frequency in ({440, 660, 880} - {own_frequency}):
        amplitude = tone_amplitude(stream, other_frequency)
        measurements[f"stream_{stream}_{other_frequency}Hz"] = round(amplitude, 4)
        if amplitude > 0.025:
            raise SystemExit(
                f"audio track {stream} contains another application's tone "
                f"({other_frequency} Hz amplitude {amplitude:.4f})"
            )

print(json.dumps({"file": str(media), "audio_streams": len(audio_streams),
                  "median_tone_amplitudes": measurements}, indent=2))
