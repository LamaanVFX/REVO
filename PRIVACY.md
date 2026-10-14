# REVO Privacy Notice

Effective October 12, 2026

REVO is a local Windows capture application. The current release has no account system,
advertising, analytics, telemetry, crash-report upload, cloud storage, or automatic media
upload.

## Data REVO handles

REVO processes the display or game/window source you select, Windows desktop audio, and—only
when you enable it—the microphone you select. This data is processed on your PC to maintain a
replay buffer and create clips or recordings.

For new installs, REVO writes the following data locally:

- clips in `%USERPROFILE%\Videos\REVO\Clips`;
- recordings in `%USERPROFILE%\Videos\REVO\Recordings`;
- settings in `%LOCALAPPDATA%\REVO\settings.ini`; and
- diagnostic logs in `%LOCALAPPDATA%\REVO\revo.log`.

If `%LOCALAPPDATA%\REVO\settings.ini` does not exist but a legacy
`%LOCALAPPDATA%\Klip\settings.ini` does, REVO copies the legacy settings file forward.
It does not delete the previous settings file or move media. Existing/custom media destinations
saved in that configuration remain selected until you change them in Settings. The
`REVO_DATA_ROOT` environment variable selects an alternate data root; `KLIP_DATA_ROOT`
remains supported as a legacy override.

The diagnostic log can contain application status, hardware/encoder names, selected capture
source labels, and error details. It does not intentionally contain recorded video or audio.

## Sharing and retention

REVO does not transmit these files to LamaanVFX or any third party. Media and diagnostics
remain until you delete them. Uninstalling REVO does not automatically delete clips,
recordings, settings, or logs.

If you choose to share a clip, recording, or diagnostic log yourself, the destination's terms
and privacy practices apply. Review logs before sharing because window/source labels may reveal
personal information.

## Changes and contact

If a future REVO release introduces network services or materially different data handling,
this notice will be updated before that behavior is released. Questions can be submitted at
https://github.com/LamaanVFX/klipedit/issues.
