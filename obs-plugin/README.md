# REVO Desktop Toast Notifications for OBS

A native Windows x64 OBS Studio plugin that displays REVO-style desktop notifications on **every connected monitor**.

These are independent Windows popups, not OBS sources or scene elements. They appear above other applications, are click-through and do not get captured into the OBS recording or stream.

## Notifications

- **RECORDING STARTED** when OBS successfully starts recording.
- **RECORDING STOPPED** when OBS finishes stopping a recording.
- **CLIP SAVED** when OBS finishes saving a Replay Buffer clip.

Each popup stays visible for 3 seconds and is placed at the top-right of each monitor. It uses a dark translucent purple card with the REVO cyan accent (#9CFFFF).

## Compatibility

Built against OBS Studio **33.0.1**, Windows x64. Use a matching build for other OBS versions.

## Install — standard OBS installation

1. Close OBS Studio.
2. Extract the ZIP into:
   `C:\ProgramData\obs-studio\plugins\`
3. Confirm the extracted layout is:
   `C:\ProgramData\obs-studio\plugins\revo-toasts\revo-toasts.dll`
   `C:\ProgramData\obs-studio\plugins\revo-toasts\data\locale\en-US.ini`
4. Start OBS Studio.

This uses the current OBS 33 plugin layout, with the DLL directly inside the plugin folder.

## Install — portable OBS

Extract the `revo-toasts` folder into the portable OBS `plugins` directory. The DLL should be directly inside `plugins\revo-toasts\`, alongside the `data` folder.

## Uninstall

Close OBS and remove the `revo-toasts` folder from the OBS `plugins` directory.
