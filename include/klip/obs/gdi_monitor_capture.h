#pragma once

namespace klip {

// Registers Klip's GDI/BitBlt monitor source with libobs. The source captures
// desktop pixels through GDI and uploads them as an OBS texture; it does not
// use Windows.Graphics.Capture or DXGI Desktop Duplication for acquisition.
void RegisterGdiMonitorCapture();

}  // namespace klip
