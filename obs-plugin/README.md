# REVO Toast Notifications for OBS

A native Windows x64 OBS Studio plugin that adds REVO-style notifications to the scene output:

- **RECORDING STARTED** when OBS successfully starts recording.
- **RECORDING STOPPED** when OBS finishes stopping a recording.
- **CLIP SAVED** when OBS finishes saving a Replay Buffer clip.

Notifications render inside the OBS scene, so they appear in the preview and in recordings/streams. The plugin places one locked overlay source into every scene in the active scene collection and follows scene changes.

## Compatibility

This package is built against OBS Studio **33.0.1** for Windows x64. It is intended for OBS 33.0.1. If using a different OBS version, use a build compiled for that version.

## Install — standard OBS installation

1. Close OBS Studio.
2. Extract the ZIP into:
   `C:\ProgramData\obs-studio\plugins\`
3. The resulting folder structure must be:
   `C:\ProgramData\obs-studio\plugins\revo-toasts\revo-toasts.dll`
   `C:\ProgramData\obs-studio\plugins\revo-toasts\data\locale\en-US.ini`
4. Start OBS Studio.

## Install — portable OBS

Extract the ZIP into the portable OBS folder's `plugins` directory. The resulting layout should be:
`<OBS folder>\plugins\revo-toasts\revo-toasts.dll`
`<OBS folder>\plugins\revo-toasts\data\locale\en-US.ini`

## Configure and preview

The plugin adds a locked **REVO Toast Notifications** source to all scenes. Select that source in the Sources dock and open Properties.

- Toggle recording notifications.
- Toggle clip-saved notifications.
- Set the display duration from 1 to 8 seconds (default: 3 seconds).
- Click **Preview notification** to show a test toast.

The card is placed at the top-right of the canvas and uses a dark translucent purple panel with the REVO cyan accent (#9CFFFF).

## Uninstall

Close OBS and delete the `revo-toasts` folder from the OBS plugins directory.
