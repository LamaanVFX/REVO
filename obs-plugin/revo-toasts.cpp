#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>

#include <obs-module.h>
#include <obs.h>
#include <graphics/graphics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "obs-frontend-api.h"

using namespace Gdiplus;

namespace {
constexpr const char *kSourceId = "revo_toast_notifications";
constexpr const char *kSourceName = "REVO Toast Notifications";
constexpr wchar_t kFrontendDll[] = L"obs-frontend-api.dll";
constexpr int kDefaultDurationSeconds = 3;

struct FrontendApi {
  HMODULE module = nullptr;
  bool owned = false;
  void (*add_event_callback)(obs_frontend_event_cb, void *) = nullptr;
  void (*remove_event_callback)(obs_frontend_event_cb, void *) = nullptr;
  void (*get_scenes)(obs_frontend_source_list *) = nullptr;
};

struct ToastSnapshot {
  bool visible = false;
  std::wstring title;
  std::wstring subtitle;
  uint64_t generation = 0;
  Color accent = Color(255, 156, 255, 255);
};

struct ToastState {
  std::mutex mutex;
  ToastSnapshot current;
  std::chrono::steady_clock::time_point expires_at{};
  std::atomic<int> duration_seconds{kDefaultDurationSeconds};
  std::atomic<bool> show_recording{true};
  std::atomic<bool> show_clip_saved{true};
};

struct OverlaySource {
  explicit OverlaySource(obs_source_t *owner) : source(owner) {}
  obs_source_t *source = nullptr;
  gs_texture_t *texture = nullptr;
  std::atomic<uint32_t> width{1920};
  std::atomic<uint32_t> height{1080};
  uint64_t rendered_generation = UINT64_MAX;
};

enum class ToastKind {
  RecordingStarted,
  RecordingStopped,
  ClipSaved,
  Preview
};

FrontendApi g_frontend;
ToastState g_toast;
obs_source_t *g_overlay_source = nullptr;
ULONG_PTR g_gdiplus_token = 0;
bool g_frontend_exit_seen = false;

template <typename T>
T ResolveFrontendProc(const char *name)
{
  return reinterpret_cast<T>(GetProcAddress(g_frontend.module, name));
}

bool LoadFrontendApi()
{
  g_frontend.module = GetModuleHandleW(kFrontendDll);
  if (!g_frontend.module) {
    g_frontend.module = LoadLibraryW(kFrontendDll);
    g_frontend.owned = g_frontend.module != nullptr;
  }
  if (!g_frontend.module)
    return false;

  g_frontend.add_event_callback =
      ResolveFrontendProc<decltype(g_frontend.add_event_callback)>("obs_frontend_add_event_callback");
  g_frontend.remove_event_callback =
      ResolveFrontendProc<decltype(g_frontend.remove_event_callback)>("obs_frontend_remove_event_callback");
  g_frontend.get_scenes =
      ResolveFrontendProc<decltype(g_frontend.get_scenes)>("obs_frontend_get_scenes");

  return g_frontend.add_event_callback && g_frontend.remove_event_callback &&
         g_frontend.get_scenes;
}

void PublishToast(ToastKind kind)
{
  if (kind == ToastKind::RecordingStarted && !g_toast.show_recording.load())
    return;
  if (kind == ToastKind::RecordingStopped && !g_toast.show_recording.load())
    return;
  if (kind == ToastKind::ClipSaved && !g_toast.show_clip_saved.load())
    return;

  ToastSnapshot next;
  switch (kind) {
  case ToastKind::RecordingStarted:
    next.title = L"RECORDING STARTED";
    next.subtitle = L"Your recording is now live.";
    next.accent = Color(255, 156, 255, 255);
    break;
  case ToastKind::RecordingStopped:
    next.title = L"RECORDING STOPPED";
    next.subtitle = L"Your recording has stopped.";
    next.accent = Color(255, 198, 164, 255);
    break;
  case ToastKind::ClipSaved:
    next.title = L"CLIP SAVED";
    next.subtitle = L"Your replay clip is ready.";
    next.accent = Color(255, 156, 255, 255);
    break;
  case ToastKind::Preview:
    next.title = L"REVO NOTIFICATIONS";
    next.subtitle = L"Your OBS overlay is working.";
    next.accent = Color(255, 156, 255, 255);
    break;
  }

  std::lock_guard<std::mutex> lock(g_toast.mutex);
  next.visible = true;
  next.generation = g_toast.current.generation + 1;
  g_toast.current = std::move(next);
  const int duration = std::clamp(g_toast.duration_seconds.load(), 1, 8);
  g_toast.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
}

ToastSnapshot ReadToastSnapshot()
{
  std::lock_guard<std::mutex> lock(g_toast.mutex);
  if (g_toast.current.visible &&
      std::chrono::steady_clock::now() >= g_toast.expires_at) {
    g_toast.current.visible = false;
    g_toast.current.title.clear();
    g_toast.current.subtitle.clear();
    ++g_toast.current.generation;
  }
  return g_toast.current;
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

void DrawToastPixels(const ToastSnapshot &toast, uint32_t width, uint32_t height,
                     std::vector<uint8_t> &pixels)
{
  pixels.assign(static_cast<size_t>(width) * height * 4, 0);
  if (!toast.visible || width < 320 || height < 180)
    return;

  Bitmap bitmap(static_cast<INT>(width), static_cast<INT>(height),
                static_cast<INT>(width * 4), PixelFormat32bppARGB, pixels.data());
  Graphics graphics(&bitmap);
  graphics.SetSmoothingMode(SmoothingModeAntiAlias);
  graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  graphics.SetCompositingMode(CompositingModeSourceOver);
  graphics.Clear(Color(0, 0, 0, 0));

  const REAL scale = std::clamp(static_cast<REAL>(width) / 1920.0F, 0.62F, 1.0F);
  const REAL card_width = 520.0F * scale;
  const REAL card_height = 132.0F * scale;
  const REAL margin = 34.0F * scale;
  const REAL x = static_cast<REAL>(width) - card_width - margin;
  const REAL y = 34.0F * scale;
  const REAL radius = 18.0F * scale;

  SolidBrush shadow(Color(95, 0, 0, 0));
  FillRoundedRectangle(graphics,
                       RectF(x + 4.0F * scale, y + 7.0F * scale, card_width,
                             card_height),
                       radius, shadow);

  SolidBrush panel(Color(232, 20, 13, 34));
  FillRoundedRectangle(graphics, RectF(x, y, card_width, card_height), radius, panel);

  SolidBrush accent(toast.accent);
  FillRoundedRectangle(graphics,
                       RectF(x + 1.0F * scale, y + 18.0F * scale,
                             5.0F * scale, card_height - 36.0F * scale),
                       2.5F * scale, accent);

  SolidBrush icon_back(Color(45, 156, 255, 255));
  graphics.FillEllipse(&icon_back, x + 28.0F * scale, y + 40.0F * scale,
                       48.0F * scale, 48.0F * scale);
  Pen icon_ring(toast.accent, 2.0F * scale);
  graphics.DrawEllipse(&icon_ring, x + 28.0F * scale, y + 40.0F * scale,
                       48.0F * scale, 48.0F * scale);

  Font brand_font(L"Segoe UI", 12.0F * scale, FontStyleBold, UnitPixel);
  Font title_font(L"Segoe UI", 29.0F * scale, FontStyleBold, UnitPixel);
  Font detail_font(L"Segoe UI", 15.0F * scale, FontStyleRegular, UnitPixel);
  SolidBrush brand_brush(Color(190, 197, 194, 211));
  SolidBrush title_shadow(Color(130, 0, 0, 0));
  SolidBrush title_brush(Color(255, 250, 248, 255));
  SolidBrush detail_brush(Color(220, 201, 196, 216));

  const REAL text_x = x + 98.0F * scale;
  graphics.DrawString(L"REVO  /  STATUS", -1, &brand_font,
                      RectF(text_x, y + 16.0F * scale, card_width - 120.0F * scale,
                            20.0F * scale),
                      nullptr, &brand_brush);
  graphics.DrawString(toast.title.c_str(), -1, &title_font,
                      RectF(text_x + 1.0F * scale, y + 40.0F * scale,
                            card_width - 116.0F * scale, 42.0F * scale),
                      nullptr, &title_shadow);
  graphics.DrawString(toast.title.c_str(), -1, &title_font,
                      RectF(text_x, y + 39.0F * scale,
                            card_width - 116.0F * scale, 42.0F * scale),
                      nullptr, &title_brush);
  graphics.DrawString(toast.subtitle.c_str(), -1, &detail_font,
                      RectF(text_x, y + 88.0F * scale,
                            card_width - 116.0F * scale, 24.0F * scale),
                      nullptr, &detail_brush);
}

void UpdateOverlayTexture(OverlaySource *overlay, const ToastSnapshot &toast)
{
  obs_video_info video_info{};
  uint32_t width = 1920;
  uint32_t height = 1080;
  if (obs_get_video_info(&video_info)) {
    if (video_info.base_width) width = video_info.base_width;
    if (video_info.base_height) height = video_info.base_height;
  }

  if (overlay->rendered_generation == toast.generation &&
      overlay->width.load() == width && overlay->height.load() == height)
    return;

  overlay->width.store(width);
  overlay->height.store(height);
  overlay->rendered_generation = toast.generation;

  std::vector<uint8_t> pixels;
  DrawToastPixels(toast, width, height, pixels);

  obs_enter_graphics();
  if (overlay->texture) {
    gs_texture_destroy(overlay->texture);
    overlay->texture = nullptr;
  }
  if (toast.visible && !pixels.empty()) {
    const uint8_t *data = pixels.data();
    overlay->texture = gs_texture_create(width, height, GS_BGRA, 1, &data, GS_DYNAMIC);
  }
  obs_leave_graphics();
}

void *CreateOverlay(obs_data_t *, obs_source_t *source)
{
  auto *overlay = new OverlaySource(source);
  UpdateOverlayTexture(overlay, ReadToastSnapshot());
  return overlay;
}

void DestroyOverlay(void *data)
{
  auto *overlay = static_cast<OverlaySource *>(data);
  if (!overlay)
    return;
  if (overlay->texture) {
    obs_enter_graphics();
    gs_texture_destroy(overlay->texture);
    obs_leave_graphics();
    overlay->texture = nullptr;
  }
  delete overlay;
}

void UpdateOverlay(void *data, obs_data_t *settings)
{
  (void)data;
  g_toast.show_recording.store(obs_data_get_bool(settings, "show_recording"));
  g_toast.show_clip_saved.store(obs_data_get_bool(settings, "show_clip_saved"));
  g_toast.duration_seconds.store(
      std::clamp(static_cast<int>(obs_data_get_int(settings, "duration_seconds")), 1, 8));
}

void SetOverlayDefaults(obs_data_t *settings)
{
  obs_data_set_default_bool(settings, "show_recording", true);
  obs_data_set_default_bool(settings, "show_clip_saved", true);
  obs_data_set_default_int(settings, "duration_seconds", kDefaultDurationSeconds);
}

bool PreviewToastButton(void *, obs_properties_t *)
{
  PublishToast(ToastKind::Preview);
  return true;
}

obs_properties_t *GetOverlayProperties(void *)
{
  obs_properties_t *properties = obs_properties_create();
  obs_properties_add_bool(properties, "show_recording",
                          obs_module_text("ShowRecordingNotifications"));
  obs_properties_add_bool(properties, "show_clip_saved",
                          obs_module_text("ShowClipSavedNotifications"));
  obs_property_t *duration = obs_properties_add_int_slider(
      properties, "duration_seconds", obs_module_text("NotificationDuration"), 1, 8, 1);
  obs_property_int_set_suffix(duration, " s");
  obs_properties_add_button(properties, "preview_toast", obs_module_text("PreviewNotification"),
                            PreviewToastButton);
  return properties;
}

void TickOverlay(void *data, float)
{
  auto *overlay = static_cast<OverlaySource *>(data);
  if (overlay)
    UpdateOverlayTexture(overlay, ReadToastSnapshot());
}

void RenderOverlay(void *data, gs_effect_t *)
{
  auto *overlay = static_cast<OverlaySource *>(data);
  if (!overlay || !overlay->texture)
    return;

  gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  gs_technique_t *technique = gs_effect_get_technique(effect, "Draw");
  const bool previous_srgb = gs_framebuffer_srgb_enabled();
  gs_enable_framebuffer_srgb(true);
  gs_technique_begin(technique);
  gs_technique_begin_pass(technique, 0);
  gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), overlay->texture);
  gs_draw_sprite(overlay->texture, 0, overlay->width.load(), overlay->height.load());
  gs_technique_end_pass(technique);
  gs_technique_end(technique);
  gs_enable_framebuffer_srgb(previous_srgb);
}

uint32_t GetOverlayWidth(void *data)
{
  return static_cast<OverlaySource *>(data)->width.load();
}

uint32_t GetOverlayHeight(void *data)
{
  return static_cast<OverlaySource *>(data)->height.load();
}

const char *GetOverlayName(void *)
{
  return obs_module_text("SourceName");
}

bool IsOverlayInScene(obs_scene_t *scene, obs_source_t *target)
{
  struct SearchContext {
    obs_source_t *target;
    bool found = false;
  } context{target, false};

  obs_scene_enum_items(
      scene,
      [](obs_scene_t *, obs_sceneitem_t *item, void *opaque) -> bool {
        auto *context = static_cast<SearchContext *>(opaque);
        if (obs_sceneitem_get_source(item) == context->target) {
          context->found = true;
          return false;
        }
        return true;
      },
      &context);
  return context.found;
}

void EnsureOverlayInScenes()
{
  if (!g_overlay_source || !g_frontend.get_scenes || g_frontend_exit_seen)
    return;

  obs_frontend_source_list scenes{};
  g_frontend.get_scenes(&scenes);
  for (size_t i = 0; i < scenes.sources.num; ++i) {
    obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]);
    if (!scene || IsOverlayInScene(scene, g_overlay_source))
      continue;

    obs_sceneitem_t *item = obs_scene_add(scene, g_overlay_source);
    if (!item)
      continue;
    vec2 position{};
    vec2_set(&position, 0.0F, 0.0F);
    obs_sceneitem_set_pos(item, &position);
    obs_sceneitem_set_locked(item, true);
    obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
  }

  for (size_t i = 0; i < scenes.sources.num; ++i)
    obs_source_release(scenes.sources.array[i]);
  da_free(scenes.sources);
}

void RemoveOverlayFromScenes()
{
  if (!g_overlay_source || !g_frontend.get_scenes)
    return;

  obs_frontend_source_list scenes{};
  g_frontend.get_scenes(&scenes);
  for (size_t i = 0; i < scenes.sources.num; ++i) {
    obs_scene_t *scene = obs_scene_from_source(scenes.sources.array[i]);
    if (!scene)
      continue;
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *opaque) -> bool {
          auto *target = static_cast<obs_source_t *>(opaque);
          if (obs_sceneitem_get_source(item) == target) {
            obs_sceneitem_remove(item);
            return false;
          }
          return true;
        },
        g_overlay_source);
  }
  for (size_t i = 0; i < scenes.sources.num; ++i)
    obs_source_release(scenes.sources.array[i]);
  da_free(scenes.sources);
}

void FrontendEvent(enum obs_frontend_event event, void *)
{
  switch (event) {
  case OBS_FRONTEND_EVENT_RECORDING_STARTED:
    PublishToast(ToastKind::RecordingStarted);
    break;
  case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
    PublishToast(ToastKind::RecordingStopped);
    break;
  case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
    PublishToast(ToastKind::ClipSaved);
    break;
  case OBS_FRONTEND_EVENT_FINISHED_LOADING:
  case OBS_FRONTEND_EVENT_SCENE_CHANGED:
  case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
  case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
    EnsureOverlayInScenes();
    break;
  case OBS_FRONTEND_EVENT_EXIT:
    RemoveOverlayFromScenes();
    g_frontend_exit_seen = true;
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
  return "REVO-style recording and replay notifications for OBS Studio.";
}

bool obs_module_load(void)
{
  GdiplusStartupInput gdiplus_input;
  if (GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr) != Ok) {
    blog(LOG_ERROR, "[REVO Toasts] GDI+ initialization failed.");
    return false;
  }

  obs_source_info source_info{};
  source_info.id = kSourceId;
  source_info.type = OBS_SOURCE_TYPE_INPUT;
  source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
  source_info.get_name = GetOverlayName;
  source_info.create = CreateOverlay;
  source_info.destroy = DestroyOverlay;
  source_info.update = UpdateOverlay;
  source_info.get_defaults = SetOverlayDefaults;
  source_info.get_properties = GetOverlayProperties;
  source_info.video_tick = TickOverlay;
  source_info.video_render = RenderOverlay;
  source_info.get_width = GetOverlayWidth;
  source_info.get_height = GetOverlayHeight;
  obs_register_source(&source_info);

  if (!LoadFrontendApi()) {
    blog(LOG_ERROR,
         "[REVO Toasts] Could not load OBS frontend API. The plugin needs OBS Studio, not bare libobs.");
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
    return false;
  }

  g_frontend.add_event_callback(FrontendEvent, nullptr);
  g_overlay_source = obs_source_create(kSourceId, kSourceName, nullptr, nullptr);
  if (!g_overlay_source) {
    g_frontend.remove_event_callback(FrontendEvent, nullptr);
    if (g_frontend.owned)
      FreeLibrary(g_frontend.module);
    g_frontend = {};
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
    blog(LOG_ERROR, "[REVO Toasts] Could not create the overlay source.");
    return false;
  }

  EnsureOverlayInScenes();
  blog(LOG_INFO, "[REVO Toasts] Loaded. Recording and replay notifications are enabled.");
  return true;
}

void obs_module_unload(void)
{
  if (!g_frontend_exit_seen) {
    if (g_frontend.remove_event_callback)
      g_frontend.remove_event_callback(FrontendEvent, nullptr);
    RemoveOverlayFromScenes();
  }

  if (g_overlay_source) {
    obs_source_release(g_overlay_source);
    g_overlay_source = nullptr;
  }

  if (g_frontend.owned && g_frontend.module)
    FreeLibrary(g_frontend.module);
  g_frontend = {};

  if (g_gdiplus_token) {
    GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
  }
}
