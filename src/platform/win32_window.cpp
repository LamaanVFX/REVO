#include "klip/platform/win32_window.h"

#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message,
                                                             WPARAM w_param, LPARAM l_param);

namespace klip {
namespace {

constexpr wchar_t kWindowClass[] = L"KlipWindowClass";
constexpr wchar_t kToastWindowClass[] = L"REVOStatusToastClass";
constexpr UINT_PTR kToastDismissTimer = 1;
constexpr UINT_PTR kToastKeepTopmostTimer = 2;
constexpr UINT kToastKeepTopmostIntervalMs = 50;
constexpr int kToastWidth = 320;
constexpr int kToastHeight = 64;
constexpr int kToastMargin = 18;
BOOL CALLBACK CollectMonitorWorkArea(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
  auto& work_areas = *reinterpret_cast<std::vector<RECT>*>(parameter);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) work_areas.push_back(info.rcWork);
  return TRUE;
}


void ApplyDarkWindowChrome(HWND window) {
  constexpr DWORD kUseImmersiveDarkMode = 20;
  constexpr DWORD kBorderColor = 34;
  constexpr DWORD kCaptionColor = 35;
  constexpr DWORD kTextColor = 36;
  const BOOL dark = TRUE;
  const COLORREF border = RGB(39, 46, 57);
  const COLORREF caption = RGB(8, 12, 19);
  const COLORREF text = RGB(238, 240, 247);
  DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &dark, sizeof(dark));
  DwmSetWindowAttribute(window, kBorderColor, &border, sizeof(border));
  DwmSetWindowAttribute(window, kCaptionColor, &caption, sizeof(caption));
  DwmSetWindowAttribute(window, kTextColor, &text, sizeof(text));
}

}  // namespace

Win32Window::~Win32Window() noexcept { Destroy(); }

bool Win32Window::Create(HINSTANCE instance, int show_command, Error& error) {
  instance_ = instance;
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_CLASSDC;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  // Use the same compiled REVO icon resource everywhere, but load it at the
  // actual Windows icon dimensions. Loading a 64 px icon for the title bar and
  // a 32 px icon for the tray made Windows repeatedly downscale it, which
  // softened the mark and made its optical alignment look off-center.
  const int large_icon_width = std::max(32, GetSystemMetrics(SM_CXICON));
  const int large_icon_height = std::max(32, GetSystemMetrics(SM_CYICON));
  const int small_icon_width = std::max(16, GetSystemMetrics(SM_CXSMICON));
  const int small_icon_height = std::max(16, GetSystemMetrics(SM_CYSMICON));
  large_icon_ = static_cast<HICON>(LoadImageW(
      instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, large_icon_width, large_icon_height,
      LR_DEFAULTCOLOR));
  small_icon_ = static_cast<HICON>(LoadImageW(
      instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, small_icon_width, small_icon_height,
      LR_DEFAULTCOLOR));
  window_class.hIcon = large_icon_;
  window_class.hIconSm = small_icon_;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    if (small_icon_ != nullptr) DestroyIcon(small_icon_);
    if (large_icon_ != nullptr) DestroyIcon(large_icon_);
    small_icon_ = nullptr;
    large_icon_ = nullptr;
    error = MakeWin32Error(ErrorComponent::kWindow, "register window class", GetLastError());
    return false;
  }
  constexpr DWORD style =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SIZEBOX;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
  const int work_width = monitor.rcWork.right - monitor.rcWork.left;
  const int work_height = monitor.rcWork.bottom - monitor.rcWork.top;
  const int client_width = std::min(1600, std::max(1120, work_width - 32));
  const int client_height = std::min(860, std::max(620, work_height - 48));
  RECT rectangle{0, 0, client_width, client_height};
  AdjustWindowRect(&rectangle, style, FALSE);
  const int width = rectangle.right - rectangle.left;
  const int height = rectangle.bottom - rectangle.top;
  const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
  const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
  window_ = CreateWindowW(kWindowClass, L"REVO", style, x, y, width, height, nullptr, nullptr,
                          instance, this);
  if (window_ == nullptr) {
    error = MakeWin32Error(ErrorComponent::kWindow, "create main window", GetLastError());
    UnregisterClassW(kWindowClass, instance_);
    return false;
  }
  EnsureToastClass();
  ApplyDarkWindowChrome(window_);
  SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon_));
  SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon_));
  visible_ = show_command != SW_HIDE;
  ShowWindow(window_, show_command);
  UpdateWindow(window_);
#if defined(KLIP_USE_LIBOBS)
  taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
  UpdateTray(true);
#endif
  return true;
}

void Win32Window::UpdateTray(bool add) noexcept {
#if defined(KLIP_USE_LIBOBS)
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon); icon.hWnd = window_; icon.uID = 1;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = WM_APP + 54; icon.hIcon = small_icon_;
  wcscpy_s(icon.szTip, L"REVO / open or quit from the tray");
  if (add) tray_added_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
  else { Shell_NotifyIconW(NIM_DELETE, &icon); tray_added_ = false; }
#else
  (void)add;
#endif
}

void Win32Window::Destroy() noexcept {
  if (tray_added_) UpdateTray(false);
  for (const HWND toast : toast_windows_) {
    if (toast && IsWindow(toast)) DestroyWindow(toast);
  }
  toast_windows_.clear();
  if (toast_class_registered_ && instance_ != nullptr) {
    UnregisterClassW(kToastWindowClass, instance_);
    toast_class_registered_ = false;
  }
  if (window_ != nullptr) {
    if (IsWindow(window_)) {
      SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
      DestroyWindow(window_);
    }
    window_ = nullptr;
  }
  if (instance_ != nullptr) {
    UnregisterClassW(kWindowClass, instance_);
    instance_ = nullptr;
  }
  if (small_icon_ != nullptr) {
    DestroyIcon(small_icon_);
    small_icon_ = nullptr;
  }
  if (large_icon_ != nullptr) {
    DestroyIcon(large_icon_);
    large_icon_ = nullptr;
  }
}

bool Win32Window::PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
    if (message.message == WM_QUIT) return false;
  }
  return true;
}

void Win32Window::ToggleVisibility() {
  visible_ = !visible_;
  ShowWindow(window_, visible_ ? SW_SHOW : SW_HIDE);
  if (visible_) SetForegroundWindow(window_);
}

void Win32Window::SetHotkeyCallback(HotkeyCallback callback) {
  hotkey_callback_ = std::move(callback);
}

bool Win32Window::EnsureToastClass() {
  if (toast_class_registered_) return true;
  if (!instance_) return false;

  WNDCLASSEXW toast_class{};
  toast_class.cbSize = sizeof(toast_class);
  toast_class.lpfnWndProc = ToastWindowProc;
  toast_class.hInstance = instance_;
  toast_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  toast_class.lpszClassName = kToastWindowClass;
  if (!RegisterClassExW(&toast_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    OutputDebugStringW(L"[REVO] Could not register toast window class.\\n");
    return false;
  }
  toast_class_registered_ = true;
  return true;
}

void Win32Window::ShowToast(const std::wstring& message) {
  if (message.empty() || !EnsureToastClass()) return;
  toast_text_ = message;

  // A matching popup appears in the top-right of every connected monitor.
  // A secondary-display copy remains useful if a borderless-fullscreen game
  // on one monitor obscures normal desktop overlays on that display.
  std::vector<RECT> work_areas;
  EnumDisplayMonitors(nullptr, nullptr, CollectMonitorWorkArea,
                      reinterpret_cast<LPARAM>(&work_areas));
  if (work_areas.empty()) {
    work_areas.push_back(
        RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)});
  }

  while (toast_windows_.size() > work_areas.size()) {
    const HWND obsolete = toast_windows_.back();
    toast_windows_.pop_back();
    if (obsolete && IsWindow(obsolete)) DestroyWindow(obsolete);
  }
  while (toast_windows_.size() < work_areas.size()) toast_windows_.push_back(nullptr);

  for (std::size_t index = 0; index < work_areas.size(); ++index) {
    HWND& toast = toast_windows_[index];
    if (!toast || !IsWindow(toast)) {
      toast = CreateWindowExW(
          WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
          kToastWindowClass, L"REVO status", WS_POPUP, 0, 0, kToastWidth, kToastHeight,
          nullptr, nullptr, instance_, this);
      if (!toast) {
        OutputDebugStringW(L"[REVO] Could not create a monitor toast popup.\\n");
        continue;
      }
      if (!SetLayeredWindowAttributes(toast, 0, 224, LWA_ALPHA))
        OutputDebugStringW(L"[REVO] Could not configure toast transparency.\\n");
    }

    const RECT& work = work_areas[index];
    const int x = std::max(work.left + kToastMargin,
                           work.right - kToastWidth - kToastMargin);
    const int y = work.top + kToastMargin;
    KillTimer(toast, kToastDismissTimer);
    KillTimer(toast, kToastKeepTopmostTimer);
    if (!SetWindowPos(toast, HWND_TOPMOST, x, y, kToastWidth, kToastHeight,
                      SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) {
      OutputDebugStringW(L"[REVO] Could not place topmost toast popup.\\n");
      continue;
    }
    ShowWindow(toast, SW_SHOWNOACTIVATE);
    SetWindowPos(toast, HWND_TOPMOST, x, y, kToastWidth, kToastHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!SetTimer(toast, kToastDismissTimer, 3000, nullptr))
      OutputDebugStringW(L"[REVO] Could not start toast dismissal timer.\\n");
    if (!SetTimer(toast, kToastKeepTopmostTimer, kToastKeepTopmostIntervalMs, nullptr))
      OutputDebugStringW(L"[REVO] Could not start toast topmost refresh timer.\\n");
    RedrawWindow(toast, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
  }
}

bool Win32Window::TakeResize(UINT& width, UINT& height) {
  if (pending_width_ == 0 || pending_height_ == 0) return false;
  width = std::exchange(pending_width_, 0);
  height = std::exchange(pending_height_, 0);
  return true;
}

LRESULT CALLBACK Win32Window::WindowProc(HWND window, UINT message, WPARAM w_param,
                                         LPARAM l_param) {
  auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    self = static_cast<Win32Window*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (self && message != WM_NCHITTEST) self->redraw_requested_ = true;
  if (ImGui::GetCurrentContext() != nullptr &&
      ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param))
    return 1;
  return self != nullptr ? self->HandleMessage(message, w_param, l_param)
                         : DefWindowProcW(window, message, w_param, l_param);
}

LRESULT CALLBACK Win32Window::ToastWindowProc(HWND window, UINT message,
                                                            WPARAM w_param, LPARAM l_param) {
  auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
    self = static_cast<Win32Window*>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  switch (message) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // Never steal clicks from the game or desktop.
    case WM_ERASEBKGND:
      return 1;
    case WM_TIMER:
      if (w_param == kToastDismissTimer) {
        KillTimer(window, kToastDismissTimer);
        KillTimer(window, kToastKeepTopmostTimer);
        ShowWindow(window, SW_HIDE);
        return 0;
      }
      if (w_param == kToastKeepTopmostTimer && self &&
          std::find(self->toast_windows_.begin(), self->toast_windows_.end(), window) !=
              self->toast_windows_.end() &&
          IsWindowVisible(window)) {
        // Keep this popup first within the topmost Z-order band, without activation.
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      HBRUSH background = CreateSolidBrush(RGB(5, 25, 31));
      HPEN outline = CreatePen(PS_SOLID, 1, RGB(0, 159, 177));
      HGDIOBJ old_brush = SelectObject(dc, background);
      HGDIOBJ old_pen = SelectObject(dc, outline);
      RoundRect(dc, client.left, client.top, client.right, client.bottom, 16, 16);
      HBRUSH accent = CreateSolidBrush(RGB(25, 232, 239));
      RECT accent_rect{client.left + 10, client.top + 14,
                       client.left + 14, client.bottom - 14};
      FillRect(dc, &accent_rect, accent);
      DeleteObject(accent);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(background);
      DeleteObject(outline);

      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(235, 249, 250));
      HFONT font = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
      if (self) {
        RECT text_rect = client;
        text_rect.left += 12;
        text_rect.right -= 12;
        DrawTextW(dc, self->toast_text_.c_str(), -1, &text_rect,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
      }
      if (old_font) SelectObject(dc, old_font);
      if (font) DeleteObject(font);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_DESTROY:
      KillTimer(window, kToastDismissTimer);
      KillTimer(window, kToastKeepTopmostTimer);
      return 0;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT Win32Window::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
#if defined(KLIP_USE_LIBOBS)
  if (taskbar_created_message_ && message == taskbar_created_message_) { UpdateTray(true); return 0; }
#endif
  switch (message) {
#if defined(KLIP_USE_LIBOBS)
    case WM_CLOSE:
      if (tray_added_) { if (visible_) ToggleVisibility(); return 0; }
      break;
    case WM_APP + 56:
      if (!visible_) ToggleVisibility();
      SetForegroundWindow(window_);
      return 0;
    case WM_APP + 54:
      if (l_param == WM_LBUTTONUP || l_param == WM_LBUTTONDBLCLK) {
        if (!visible_) ToggleVisibility();
        SetForegroundWindow(window_);
      } else if (l_param == WM_RBUTTONUP) {
        POINT position{}; GetCursorPos(&position);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Open REVO");
        AppendMenuW(menu, MF_STRING, 2, L"Quit REVO");
        SetForegroundWindow(window_);
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, position.x, position.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command == 1) { if (!visible_) ToggleVisibility(); SetForegroundWindow(window_); }
        else if (command == 2) { UpdateTray(false); DestroyWindow(window_); }
      }
      return 0;
#endif
    case WM_GETMINMAXINFO: {
      auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
      RECT minimum{0, 0, 960, 620};
      AdjustWindowRect(
          &minimum,
          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SIZEBOX,
          FALSE);
      limits->ptMinTrackSize.x = minimum.right - minimum.left;
      limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
      return 0;
    }
    case WM_SIZE:
      minimized_ = w_param == SIZE_MINIMIZED;
      if (w_param != SIZE_MINIMIZED) {
        pending_width_ = LOWORD(l_param);
        pending_height_ = HIWORD(l_param);
      }
      return 0;
    case WM_HOTKEY:
      if (hotkey_callback_) hotkey_callback_(static_cast<int>(w_param));
      return 0;
    case WM_SYSCOMMAND:
      if ((w_param & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window_, message, w_param, l_param);
}

}  // namespace klip
