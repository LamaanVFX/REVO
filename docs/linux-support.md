# Linux port: scope and release gates

Linux is not yet a supported REVO desktop release. The Windows installer cannot
be run natively on Linux. Wine/Proton is not an appropriate replacement for the
capture/audio backends, and this work does not advertise Wine compatibility.

Shared configuration, replay-budget policy and deterministic core tests now have
an Ubuntu 24.04 CI job. A green core job is not evidence of working screen capture.
The current Windows app uses Win32 windows/hotkeys, D3D11, WASAPI, Windows OBS
modules and Windows output-file reservation. These must be replaced, not stubbed.

Initial target for the native port: Ubuntu 24.04+ x86_64. Confirm the intended
desktop/session before promising a download. The native libobs engine can be
reused, but Windows texture-encoder IDs and paths cannot.

1. Separate portable OBS output/encoder lifecycle from Windows source selection,
   message dispatch and D3D11 setup. Keep the tested Windows backend intact.
2. Add an SDL/OpenGL ImGui host and POSIX output reservation, with the same
   dashboard/settings workflows. Preserve bounded save queues and graceful quit.
3. Wayland: request capture through the XDG Desktop Portal/PipeWire OBS source.
   Always respect the user's portal permission and source selection. X11: use
   native OBS XSHM/XComposite sources; no Windows game-hook assumption.
4. Audio: native OBS PulseAudio/PipeWire desktop and microphone sources, with
   volume and separate-track mapping. Validate actual sound and device reconnects.
5. Encoding: enumerate native Linux NVENC/VAAPI/x264 capabilities, test encoder
   creation, and apply the same duration-first budget. Do not assume Windows
   encoder identifiers or claim hardware support based only on enumeration.
6. Shortcuts: integrate session-supported global shortcuts (including the portal
   where available), report unavailable/reserved bindings clearly, and offer
   visible controls. Never bypass Wayland's security model.
7. Package dependencies/licenses/sources consistently. Test fresh installs and
   uninstall on the target distro, plus launcher and desktop integration.

Release gates: a real Wayland desktop and X11 desktop, 30/60/120 FPS recordings
where the source/hardware supports them, full warmed-up replay duration under
high-motion content, clip-while-recording, decoded cadence and A/V sync, denied
portal permission, source/device changes, invalid output folder, repeated saves,
global hotkeys, shutdown and bounded memory. Linux GPU hardware must be tested;
headless CI/containers alone cannot establish desktop capture compatibility.

This development PC currently has no WSL installation, and the existing Docker
Linux engine returned an error. No kernel feature, driver, privileged container
or security setting was changed to work around this. A native Linux test machine
or an explicitly approved Linux environment is needed for desktop verification.
