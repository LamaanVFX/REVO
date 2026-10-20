#include "klip/obs/gdi_monitor_capture.h"

#include <windows.h>

#include <graphics/graphics.h>
#include <obs.h>
#include <obs-source.h>
#include <util/base.h>
#include <util/platform.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace klip {
namespace {

constexpr char kSourceId[] = "klip_gdi_monitor_capture";
constexpr char kMonitorIdKey[] = "monitor_id";
constexpr char kCaptureCursorKey[] = "capture_cursor";

struct MonitorInfo {
  HMONITOR handle = nullptr;
  RECT bounds{};
  std::string gdi_name;
  bool found = false;
};

struct MonitorLookup {
  const char* requested = nullptr;
  MonitorInfo result;
};

BOOL CALLBACK FindMonitorCallback(HMONITOR monitor, HDC, LPRECT bounds, LPARAM parameter) {
  auto& lookup = *reinterpret_cast<MonitorLookup*>(parameter);
  MONITORINFOEXA info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoA(monitor, reinterpret_cast<MONITORINFO*>(&info))) return TRUE;

  bool match = _stricmp(info.szDevice, lookup.requested) == 0;
  DISPLAY_DEVICEA display{};
  display.cb = sizeof(display);
  if (EnumDisplayDevicesA(info.szDevice, 0, &display, EDD_GET_DEVICE_INTERFACE_NAME)) {
    match = match || _stricmp(display.DeviceID, lookup.requested) == 0;
  }
  if (!match) return TRUE;

  lookup.result.handle = monitor;
  lookup.result.bounds = *bounds;
  lookup.result.gdi_name = info.szDevice;
  lookup.result.found = true;
  return FALSE;
}

MonitorInfo FindMonitor(const char* requested) {
  MonitorLookup lookup{};
  lookup.requested = requested;
  if (requested && *requested) {
    EnumDisplayMonitors(nullptr, nullptr, FindMonitorCallback, reinterpret_cast<LPARAM>(&lookup));
  }
  return lookup.result;
}

struct GdiMonitorCapture {
  std::mutex mutex;
  HDC screen_dc = nullptr;
  HDC memory_dc = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ previous_bitmap = nullptr;
  std::uint8_t* pixels = nullptr;
  gs_texture_t* texture = nullptr;
  MonitorInfo monitor;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool capture_cursor = true;
  bool has_frame = false;
  bool logged_capture_error = false;
};

void ReleaseGdiResources(GdiMonitorCapture* capture) {
  if (!capture) return;
  if (capture->memory_dc && capture->previous_bitmap && capture->previous_bitmap != HGDI_ERROR) {
    SelectObject(capture->memory_dc, capture->previous_bitmap);
  }
  if (capture->dib) {
    DeleteObject(capture->dib);
    capture->dib = nullptr;
  }
  if (capture->memory_dc) {
    DeleteDC(capture->memory_dc);
    capture->memory_dc = nullptr;
  }
  if (capture->screen_dc) {
    ReleaseDC(nullptr, capture->screen_dc);
    capture->screen_dc = nullptr;
  }
  capture->pixels = nullptr;
}

void DestroyCaptureOnGraphicsThread(void* data) {
  auto* capture = static_cast<GdiMonitorCapture*>(data);
  if (capture->texture) {
    gs_texture_destroy(capture->texture);
    capture->texture = nullptr;
  }
  ReleaseGdiResources(capture);
  delete capture;
}

const char* GdiMonitorGetName(void*) { return "Display Capture (GDI — no WGC/DXGI)"; }

void* GdiMonitorCreate(obs_data_t* settings, obs_source_t*) {
  const char* monitor_id = obs_data_get_string(settings, kMonitorIdKey);
  auto monitor = FindMonitor(monitor_id);
  if (!monitor.found) {
    blog(LOG_WARNING, "[klip_gdi_monitor_capture] Could not resolve monitor id '%s'", monitor_id);
    return nullptr;
  }

  auto* capture = new GdiMonitorCapture();
  capture->monitor = std::move(monitor);
  capture->width = static_cast<std::uint32_t>(capture->monitor.bounds.right - capture->monitor.bounds.left);
  capture->height = static_cast<std::uint32_t>(capture->monitor.bounds.bottom - capture->monitor.bounds.top);
  capture->capture_cursor = obs_data_get_bool(settings, kCaptureCursorKey);
  if (!capture->width || !capture->height) {
    blog(LOG_WARNING, "[klip_gdi_monitor_capture] Monitor has invalid dimensions");
    delete capture;
    return nullptr;
  }

  capture->screen_dc = GetDC(nullptr);
  if (capture->screen_dc) capture->memory_dc = CreateCompatibleDC(capture->screen_dc);

  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = static_cast<LONG>(capture->width);
  bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(capture->height);  // top-down DIB
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;

  if (capture->screen_dc && capture->memory_dc) {
    capture->dib = CreateDIBSection(capture->screen_dc, &bitmap_info, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&capture->pixels), nullptr, 0);
    if (capture->dib) capture->previous_bitmap = SelectObject(capture->memory_dc, capture->dib);
  }

  if (!capture->screen_dc || !capture->memory_dc || !capture->dib || !capture->pixels ||
      !capture->previous_bitmap || capture->previous_bitmap == HGDI_ERROR) {
    blog(LOG_WARNING, "[klip_gdi_monitor_capture] Failed to initialize GDI resources (Win32 error %lu)",
         GetLastError());
    ReleaseGdiResources(capture);
    delete capture;
    return nullptr;
  }

  SetStretchBltMode(capture->memory_dc, COLORONCOLOR);
  blog(LOG_INFO, "[klip_gdi_monitor_capture] Capturing %s at %ux%u with GDI BitBlt (no WGC/DXGI duplication)",
       capture->monitor.gdi_name.c_str(), capture->width, capture->height);
  return capture;
}

void GdiMonitorTick(void* data, float) {
  auto* capture = static_cast<GdiMonitorCapture*>(data);
  std::lock_guard lock(capture->mutex);
  capture->has_frame = false;

  const auto& rect = capture->monitor.bounds;
  if (!BitBlt(capture->memory_dc, 0, 0, static_cast<int>(capture->width),
              static_cast<int>(capture->height), capture->screen_dc, rect.left, rect.top,
              SRCCOPY | CAPTUREBLT)) {
    if (!capture->logged_capture_error) {
      blog(LOG_WARNING, "[klip_gdi_monitor_capture] BitBlt failed for %s (Win32 error %lu)",
           capture->monitor.gdi_name.c_str(), GetLastError());
      capture->logged_capture_error = true;
    }
    return;
  }
  capture->logged_capture_error = false;

  if (capture->capture_cursor) {
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) &&
        PtInRect(&rect, cursor.ptScreenPos)) {
      ICONINFO icon{};
      if (GetIconInfo(cursor.hCursor, &icon)) {
        const int x = cursor.ptScreenPos.x - rect.left - static_cast<int>(icon.xHotspot);
        const int y = cursor.ptScreenPos.y - rect.top - static_cast<int>(icon.yHotspot);
        DrawIconEx(capture->memory_dc, x, y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
      }
    }
  }

  // GDI's 32-bit DIB leaves the alpha byte undefined/zero; mark pixels opaque
  // so OBS doesn't make the captured desktop transparent during composition.
  const std::size_t pixel_count = static_cast<std::size_t>(capture->width) * capture->height;
  for (std::size_t i = 0; i < pixel_count; ++i) capture->pixels[i * 4 + 3] = 0xFF;
  capture->has_frame = true;
}

void GdiMonitorRender(void* data, gs_effect_t*) {
  auto* capture = static_cast<GdiMonitorCapture*>(data);
  std::lock_guard lock(capture->mutex);
  if (!capture->pixels || !capture->has_frame) return;

  if (!capture->texture) {
    capture->texture = gs_texture_create(capture->width, capture->height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
    if (!capture->texture) {
      blog(LOG_WARNING, "[klip_gdi_monitor_capture] Could not create GPU texture for %s",
           capture->monitor.gdi_name.c_str());
      return;
    }
  }

  gs_texture_set_image(capture->texture, capture->pixels, capture->width * 4, false);
  obs_source_draw(capture->texture, 0, 0, capture->width, capture->height, false);
}

std::uint32_t GdiMonitorGetWidth(void* data) {
  return static_cast<GdiMonitorCapture*>(data)->width;
}

std::uint32_t GdiMonitorGetHeight(void* data) {
  return static_cast<GdiMonitorCapture*>(data)->height;
}

void GdiMonitorDestroy(void* data) {
  if (data) obs_queue_task(OBS_TASK_GRAPHICS, DestroyCaptureOnGraphicsThread, data, false);
}

}  // namespace

void RegisterGdiMonitorCapture() {
  // OBS clears registered source types during obs_shutdown(); register this
  // source on every ObsEngine initialization, just like the modules do.
  obs_source_info info{};
  info.id = kSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  // This source renders one texture via obs_source_draw(), which requires
  // libobs's normal active effect. CUSTOM_DRAW disables that setup and can
  // leave gs_get_effect() null, producing a black source.
  info.output_flags = OBS_SOURCE_VIDEO;
  info.get_name = GdiMonitorGetName;
  info.create = GdiMonitorCreate;
  info.destroy = GdiMonitorDestroy;
  info.get_width = GdiMonitorGetWidth;
  info.get_height = GdiMonitorGetHeight;
  info.video_tick = GdiMonitorTick;
  info.video_render = GdiMonitorRender;
  obs_register_source(&info);
}

}  // namespace klip
