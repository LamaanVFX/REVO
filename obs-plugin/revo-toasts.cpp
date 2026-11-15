#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>

#include <obs-module.h>
#include <obs.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "obs-frontend-api.h"

using namespace Gdiplus;

namespace {
constexpr wchar_t kWindowClass[] = L"REVO.ToastOverlay.Window";
constexpr wchar_t kWindowTitle[] = L"REVO Toast";
constexpr UINT kToastMessage = WM_APP + 0x431;
constexpr UINT_PTR kToastTimer = 0x431;
constexpr int kToastDurationMs = 3000;
constexpr int kToastWidth = 520;
constexpr int kToastHeight = 132;
constexpr int kToastMargin = 26;

struct FrontendApi {
  HMODULE module = nullptr;
  bool owned = false;
  void (*add_event_callback)(obs_frontend_event_cb, void *) = nullptr;
  void (*remove_event_callback)(obs_frontend_event_cb, void *) = nullptr;
};

struct ToastMessage {
  std::wstring title;
  std::wstring subtitle;
  Color accent = Color(255, 156, 255, 255);
};

FrontendApi g_frontend;
ULONG_PTR g_gdiplus_token = 0;
std::thread g_toast_thread;
std::atomic<DWORD> g_toast_thread_id{0};
std::mutex g_thread_mutex;
std::condition_variable g_thread_ready;
bool g_thread_started = false;
std::atomic<bool> g_shutting_down{false};

template <typename T>
T ResolveFrontendProc(const char *name)
{
  return reinterpret_cast<T>(GetProcAddress(g_frontend.module, name));
}

bool LoadFrontendApi()
{
  g_frontend.module = GetModuleHandleW(L"obs-frontend-api.dll");
  if (!g_frontend.module) {
    g_frontend.module = LoadLibraryW(L"obs-frontend-api.dll");
    g_frontend.owned = g_frontend.module != nullptr;
  }
  if (!g_frontend.module)
    return false;

  g_frontend.add_event_callback =
      ResolveFrontendProc<decltype(g_frontend.add_event_callback)>("obs_frontend_add_event_callback");
  g_frontend.remove_event_callback =
      ResolveFrontendProc<decltype(g_frontend.remove_event_callback)>("obs_frontend_remove_event_callback");

  return g_frontend.add_event_callback && g_frontend.remove_event_callback;
}

void FillRoundedRectangle(Graphics &graphics, const RectF &rect, REAL radius,
                          const Brush &brush)
{
  GraphicsPath path;
  const REAL diameter = radius * 2.0F;
  path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0F, 90.0F);
  path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0F, 90.0F);
  path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter,
              diameter, 0.0F, 90.0F);
  path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0F, 90.0F);
  path.CloseFigure();
  graphics.FillPath(const_cast<Brush *>(&brush), &path);
}

HBITMAP CreateToastBitmap(const ToastMessage &toast)
{
  Bitmap bitmap(kToastWidth, kToastHeight, PixelFormat32bppPARGB);
  Graphics graphics(&bitmap);
  graphics.SetSmoothingMode(SmoothingModeAntiAlias);
  graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  graphics.SetCompositingMode(CompositingModeSourceOver);
  graphics.Clear(Color(0, 0, 0, 0));

  SolidBrush shadow(Color(95, 0, 0, 0));
  FillRoundedRectangle(graphics, RectF(5.0F, 8.0F, 508.0F, 118.0F), 18.0F, shadow);

  SolidBrush panel(Color(235, 20, 13, 34));
  FillRoundedRectangle(graphics, RectF(0.0F, 0.0F, 508.0F, 118.0F), 18.0F, panel);

  SolidBrush accent(toast.accent);
  FillRoundedRectangle(graphics, RectF(0.0F, 20.0F, 5.0F, 78.0F), 2.5F, accent);

  SolidBrush icon_back(Color(45, 156, 255, 255));
  graphics.FillEllipse(&icon_back, 26.0F, 35.0F, 48.0F, 48.0F);
  Pen icon_ring(toast.accent, 2.0F);
  graphics.DrawEllipse(&icon_ring, 26.0F, 35.0F, 48.0F, 48.0F);

  Font brand_font(L"Segoe UI", 12.0F, FontStyleBold, UnitPixel);
  Font title_font(L"Segoe UI", 29.0F, FontStyleBold, UnitPixel);
  Font detail_font(L"Segoe UI", 15.0F, FontStyleRegular, UnitPixel);
  SolidBrush brand_brush(Color(190, 197, 194, 211));
  SolidBrush title_brush(Color(255, 250, 248, 255));
  SolidBrush detail_brush(Color(220, 201, 196, 216));

  const REAL text_x = 96.0F;
  graphics.DrawString(L"REVO  /  STATUS", -1, &brand_font,
                      RectF(text_x, 13.0F, 392.0F, 20.0F), nullptr, &brand_brush);
  graphics.DrawString(toast.title.c_str(), -1, &title_font,
                      RectF(text_x, 35.0F, 394.0F, 40.0F), nullptr, &title_brush);
  graphics.DrawString(toast.subtitle.c_str(), -1, &detail_font,
                      RectF(text_x, 83.0F, 394.0F, 24.0F), nullptr, &detail_brush);

  HBITMAP result = nullptr;
  bitmap.GetHBITMAP(Color(0, 0, 0, 0), &result);
  return result;
}

void ShowToastOnWindow(HWND window, const ToastMessage &toast)
{
  HBITMAP bitmap = CreateToastBitmap(toast);
  if (!bitmap)
    return;

  HDC screen_dc = GetDC(nullptr);
  HDC memory_dc = CreateCompatibleDC(screen_dc);
  HGDIOBJ previous = SelectObject(memory_dc, bitmap);

  RECT monitor{};
  HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(handle, &info)) {
    POINT destination{
        info.rcWork.right - kToastWidth - kToastMargin,
        info.rcWork.top + kToastMargin
    };
    SIZE size{kToastWidth, kToastHeight};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(window, screen_dc, &destination, &size, memory_dc,
                        &source, 0, &blend, ULW_ALPHA);
  }

  SelectObject(memory_dc, previous);
  DeleteDC(memory_dc);
  ReleaseDC(nullptr, screen_dc);
  DeleteObject(bitmap);
}

LRESULT CALLBACK ToastWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
  switch (message) {
  case WM_NCHITTEST:
    return HTTRANSPARENT;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_ERASEBKGND:
    return 1;
  case WM_TIMER:
    if (wparam == kToastTimer) {
      KillTimer(window, kToastTimer);
      DestroyWindow(window);
      return 0;
    }
    break;
  case WM_NCDESTROY:
    KillTimer(window, kToastTimer);
    break;
  default:
    break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

BOOL CALLBACK CreateToastForMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
  const auto *toast = reinterpret_cast<const ToastMessage *>(parameter);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info))
    return TRUE;

  const int x = info.rcWork.right - kToastWidth - kToastMargin;
  const int y = info.rcWork.top + kToastMargin;
  HWND window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      kWindowClass, kWindowTitle, WS_POPUP, x, y, kToastWidth, kToastHeight,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window)
    return TRUE;

  ShowToastOnWindow(window, *toast);
  SetTimer(window, kToastTimer, kToastDurationMs, nullptr);
  ShowWindow(window, SW_SHOWNOACTIVATE);
  return TRUE;
}

void DestroyToastWindows()
{
  HWND window = FindWindowW(kWindowClass, kWindowTitle);
  while (window) {
    DestroyWindow(window);
    window = FindWindowW(kWindowClass, kWindowTitle);
  }
}

bool RegisterToastWindowClass()
{
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = ToastWindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWindowClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void ToastThreadMain()
{
  MSG initial{};
  PeekMessageW(&initial, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

  if (!RegisterToastWindowClass()) {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    g_thread_started = false;
    g_thread_ready.notify_all();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    g_toast_thread_id.store(GetCurrentThreadId(), std::memory_order_release);
    g_thread_started = true;
  }
  g_thread_ready.notify_all();

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == kToastMessage) {
      std::unique_ptr<ToastMessage> toast(reinterpret_cast<ToastMessage *>(message.lParam));
      if (!toast || g_shutting_down.load(std::memory_order_acquire))
        continue;

      // Reset an earlier toast and show the new status on every connected monitor.
      DestroyToastWindows();
      EnumDisplayMonitors(nullptr, nullptr, CreateToastForMonitor,
                          reinterpret_cast<LPARAM>(toast.get()));
      continue;
    }

    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  DestroyToastWindows();
  UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
  g_toast_thread_id.store(0, std::memory_order_release);
}

bool StartToastThread()
{
  {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    g_thread_started = false;
  }

  g_toast_thread = std::thread(ToastThreadMain);
  std::unique_lock<std::mutex> lock(g_thread_mutex);
  const bool started = g_thread_ready.wait_for(
      lock, std::chrono::seconds(5), [] { return g_thread_started; });
  lock.unlock();
  if (!started && g_toast_thread.joinable())
    g_toast_thread.join();
  return started;
}

void StopToastThread()
{
  g_shutting_down.store(true, std::memory_order_release);
  const DWORD thread_id = g_toast_thread_id.load(std::memory_order_acquire);
  if (thread_id)
    PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
  if (g_toast_thread.joinable())
    g_toast_thread.join();
}

void PublishToast(const wchar_t *title, const wchar_t *subtitle, Color accent)
{
  const DWORD thread_id = g_toast_thread_id.load(std::memory_order_acquire);
  if (!thread_id || g_shutting_down.load(std::memory_order_acquire))
    return;

  auto *toast = new (std::nothrow) ToastMessage();
  if (!toast)
    return;
  toast->title = title;
  toast->subtitle = subtitle;
  toast->accent = accent;

  if (!PostThreadMessageW(thread_id, kToastMessage, 0, reinterpret_cast<LPARAM>(toast)))
    delete toast;
}

void FrontendEvent(enum obs_frontend_event event, void *)
{
  switch (event) {
  case OBS_FRONTEND_EVENT_RECORDING_STARTED:
    PublishToast(L"RECORDING STARTED", L"Your recording is now live.",
                 Color(255, 156, 255, 255));
    break;
  case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
    PublishToast(L"RECORDING STOPPED", L"Your recording has stopped.",
                 Color(255, 198, 164, 255));
    break;
  case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
    PublishToast(L"CLIP SAVED", L"Your replay clip is ready.",
                 Color(255, 156, 255, 255));
    break;
  default:
    break;
  }
}
} // namespace

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("revo-toasts", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
  return "REVO-style desktop recording and replay notifications for all monitors.";
}

bool obs_module_load(void)
{
  GdiplusStartupInput gdiplus_input;
  if (GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr) != Ok) {
    blog(LOG_ERROR, "[REVO Toasts] GDI+ initialization failed.");
    return false;
  }

  if (!LoadFrontendApi()) {
    blog(LOG_ERROR, "[REVO Toasts] Could not load OBS frontend API.");
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
    return false;
  }

  g_shutting_down.store(false, std::memory_order_release);
  if (!StartToastThread()) {
    blog(LOG_ERROR, "[REVO Toasts] Could not start the desktop toast window thread.");
    if (g_frontend.owned)
      FreeLibrary(g_frontend.module);
    g_frontend = {};
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
    return false;
  }

  g_frontend.add_event_callback(FrontendEvent, nullptr);
  blog(LOG_INFO, "[REVO Toasts] Loaded. Desktop notifications will appear on every connected monitor.");
  return true;
}

void obs_module_unload(void)
{
  if (g_frontend.remove_event_callback)
    g_frontend.remove_event_callback(FrontendEvent, nullptr);

  StopToastThread();

  if (g_frontend.owned && g_frontend.module)
    FreeLibrary(g_frontend.module);
  g_frontend = {};

  if (g_gdiplus_token) {
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
  }
}
