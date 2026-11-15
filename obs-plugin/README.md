# REVO Toast Notifications for OBS

A native OBS Studio plugin that displays REVO-style notifications in the scene output:

- **RECORDING STARTED** when OBS successfully starts recording.
- **RECORDING STOPPED** when OBS finishes stopping a recording.
- **CLIP SAVED** when OBS finishes saving a Replay Buffer clip.

Notifications render into the OBS scene itself, so they appear in the OBS preview and in recordings/streams. The plugin automatically adds a locked overlay source to every scene in the active scene collection and follows scene changes.

## Install

1. Close OBS Studio.
2. Extract the ZIP at the root of your OBS Studio installation folder.
3. Confirm these files exist:
   - obs-plugins/64bit/revo-toasts.dll
   - data/obs-plugins/revo-toasts/locale/en-US.ini
4. Start OBS Studio again.

For a standard installer installation, the destination is usually:
C:\Program Files\obs-studio\

## Configure

In the OBS Sources dock, select **REVO Toast Notifications** and open Properties.

- Toggle recording notifications.
- Toggle clip-saved notifications.
- Set the duration from 1 to 8 seconds (default: 3 seconds).
- Click **Preview notification** to test the overlay.

The overlay is placed at the top-right of the canvas. It uses a dark translucent purple panel with REVO cyan accents.

## Compatibility

This build is compiled against the OBS Studio 32.1.2 libobs SDK and is intended for Windows x64 OBS Studio. It uses the OBS frontend event API for recording and replay notifications. Do not install this build into a portable OBS copy with a different architecture.

If OBS reports that the module failed to load, remove obs-plugins/64bit/revo-toasts.dll and share the OBS log so the plugin can be rebuilt against that exact OBS SDK.

## Uninstall

Close OBS and delete:
- obs-plugins/64bit/revo-toasts.dll
- data/obs-plugins/revo-toasts/
