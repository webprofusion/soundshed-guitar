/*=============================================================================
   Elements' host layer for Android.

   Modelled on Nano Q's UIKit host (nanoq/ios/elements_host/base_view.mm) and
   compiled into Soundshed Nano Q in place of Elements' own on Android: QPlug
   compiles the host layer into each plugin from Q_PLUG_ELEMENTS_HOST_SOURCES,
   and nanoq/android/cmake/NanoQAndroid.cmake points that at this folder.
   Elements itself is configured with its headless host, which needs no
   system libraries; these sources stand in for that host's. See
   docs/plans/nano-q-android.md.

   Drawing is Artist's Skia backend on the GPU (Ganesh on OpenGL ES 3,
   through EGL), into the Surface of the activity's SurfaceView. The window is
   kept in a GPU surface of its own between frames, and only what was
   invalidated is drawn into it; once a frame that surface is copied to the
   Surface and presented. Input, as on iOS:

   - The first finger is the left mouse button: the cursor moves to it, then
     click down, drag, click up. Other fingers are ignored while it is down.
   - A finger held still for half a second is a right click. The left click
     it began is released far outside the view first, so the control it
     landed on takes it as a press dragged away and does nothing.
   - Two fingers pan as a scroll wheel does, in the natural direction.
   - A hardware keyboard sends keys and text; a mouse wheel scrolls.

   Everything runs on the app's main thread (android_host.hpp).

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
#include "android_host.hpp"

#include <elements/base_view.hpp>
#include <elements/support/error_handler.hpp>
#include <artist/resources.hpp>
#include <artist/font.hpp>
#include <infra/filesystem.hpp>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>

#include <SkCanvas.h>
#include <SkColorSpace.h>
#include <SkImage.h>
#include <SkSurface.h>
#include <ganesh/GrBackendSurface.h>
#include <ganesh/GrDirectContext.h>
#include <ganesh/SkSurfaceGanesh.h>
#include <ganesh/gl/GrGLBackendSurface.h>
#include <ganesh/gl/GrGLDirectContext.h>
#include <ganesh/gl/GrGLInterface.h>
#include <ganesh/gl/GrGLTypes.h>
#include <ganesh/gl/egl/GrGLMakeEGLInterface.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

#if !defined(ARTIST_SKIA)
# error The Android host draws with Artist's Skia backend only
#endif

namespace ph = cycfi::elements;
namespace fs = cycfi::fs;
using cycfi::artist::canvas;

namespace
{
   constexpr char const* log_tag = "NanoQ";

   // printf-style, checked by the compiler (and literal formats, which the
   // NDK's -Werror=format-security insists on).
   [[gnu::format(printf, 1, 2)]] void log_error(char const* format, ...)
   {
      va_list args;
      va_start(args, format);
      __android_log_vprint(ANDROID_LOG_ERROR, log_tag, format, args);
      va_end(args);
   }

   [[gnu::format(printf, 1, 2)]] void log_info(char const* format, ...)
   {
      va_list args;
      va_start(args, format);
      __android_log_vprint(ANDROID_LOG_INFO, log_tag, format, args);
      va_end(args);
   }
}

///////////////////////////////////////////////////////////////////////////////
// Resources: the folder the app unpacks its assets into, named by
// SOUNDSHED_NANOQ_RESOURCES (NanoQActivity sets it before the plugin loads).
// The fonts sit at its top, as in an iOS bundle; Artist's Skia backend finds
// them there by file (nanoq/android/patches/elements.patch).
namespace
{
   fs::path resource_path()
   {
      if (char const* root = std::getenv("SOUNDSHED_NANOQ_RESOURCES"); root && *root)
         return fs::path{root};
      return {};
   }
}

namespace cycfi::elements
{
   void init_resources()
   {
      static bool const done = []
      {
         if (auto path = resource_path(); !path.empty())
            cycfi::artist::add_search_path(path);
         return true;
      }();
      (void) done;
   }
}

namespace cycfi::artist
{
   void init_paths()
   {
      if (auto path = resource_path(); !path.empty())
         add_search_path(path);
   }

   // Declared in font.hpp
   fs::path get_user_fonts_directory()
   {
      return resource_path();
   }
}

namespace cycfi::elements
{
   // Defined in key.cpp
   key_code    translate_key(int android_key_code);
   int         translate_meta(int android_meta);

   // The host's own state for one base_view: what UIKit's ElementsView holds
   // on iOS. The window it sits in is the activity's.
   struct host_view
   {
      base_view*                 owner = nullptr;
      android::window*           window = nullptr;

      std::map<key_code, key_action> keys;

      // The view as Elements knows it: opened once there is a size.
      bool                       opened = false;
      extent                     size = {0, 0};
      float                      scale = 1;

      // The finger that is the mouse, and where it is (logical units).
      std::optional<int>         touch_id;
      point                      touch_start = {0, 0};
      point                      cursor = {0, 0};
      double                     touch_time = 0;
      bool                       long_press_fired = false;
      bool                       long_press_armed = false;
      int                        tap_count = 1;
      double                     last_tap_time = -1;
      point                      last_tap_at = {0, 0};

      // The two-finger pan, as scroll wheel deltas.
      bool                       panning = false;
      point                      pan_last = {0, 0};

      // What was invalidated since the last draw: each area apart, or all.
      std::vector<rect>          pending_rects;
      bool                       pending_full = true;
      bool                       needs_render = true;
   };
}

///////////////////////////////////////////////////////////////////////////////
// The activity's window: the Surface, EGL and Skia on it, and the one view
// in it.
namespace cycfi::elements::android
{
   struct window
   {
      float                      density = 1;
      ANativeWindow*             surface = nullptr;
      int                        width = 0;     // pixels
      int                        height = 0;

      EGLDisplay                 display = EGL_NO_DISPLAY;
      EGLConfig                  config = nullptr;
      EGLContext                 context = EGL_NO_CONTEXT;
      EGLSurface                 egl_surface = EGL_NO_SURFACE;
      sk_sp<GrDirectContext>     gr;
      sk_sp<SkSurface>           back;          // the window as last drawn
      sk_sp<SkSurface>           front;         // the Surface's framebuffer

      host_view*                 view = nullptr;
      bool                       focused = true;
   };
}

namespace
{
   namespace an = cycfi::elements::android;

   // How long a still finger takes to become a right click, and how far it
   // may wander first, in logical units.
   constexpr double  long_press_seconds = 0.5;
   constexpr float   long_press_slop = 10;

   // Two taps this close in time and place are a double click.
   constexpr double  double_tap_seconds = 0.35;
   constexpr float   double_tap_slop = 24;

   // Where a cancelled left click is released: outside any view.
   constexpr float   far_away = -1e6f;

   // The logical units the view works in: the display's density, but no more
   // than leaves the layout 480 units of height, the least Nano Q's compact
   // layout is made for (an 800x480 kiosk screen). A phone held sideways is
   // about 400 dp high, so it gets a little more room than its dp, at a
   // slightly smaller size; a tablet keeps its density.
   float scale_for(an::window const& w)
   {
      float scale = w.density > 0 ? w.density : 1;
      if (w.height > 0)
         scale = std::min(scale, float(w.height) / 480.0f);
      return std::max(scale, 1.0f);
   }

   bool handle_key(std::map<ph::key_code, ph::key_action>& keys, ph::base_view& view, ph::key_info k)
   {
      using ph::key_action;
      bool repeated = false;

      if (k.action == key_action::release && keys[k.key] == key_action::release)
         return false;

      if (k.action == key_action::press && keys[k.key] == key_action::press)
         repeated = true;

      keys[k.key] = k.action;

      if (repeated)
         k.action = key_action::repeat;

      return view.key(k);
   }

   ph::mouse_button button(ph::mouse_button::what what, ph::point pos, bool down, int clicks)
   {
      return {down, clicks, what, 0, pos};
   }

   // ── EGL and Skia ─────────────────────────────────────────────────────────────

   void release_gpu(an::window& w)
   {
      w.front.reset();
      w.back.reset();
      if (w.gr)
      {
         if (w.display != EGL_NO_DISPLAY && w.context != EGL_NO_CONTEXT && w.egl_surface != EGL_NO_SURFACE)
            eglMakeCurrent(w.display, w.egl_surface, w.egl_surface, w.context);
         w.gr->releaseResourcesAndAbandonContext();
         w.gr.reset();
      }
      if (w.display != EGL_NO_DISPLAY)
      {
         eglMakeCurrent(w.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
         if (w.egl_surface != EGL_NO_SURFACE)
            eglDestroySurface(w.display, w.egl_surface);
         if (w.context != EGL_NO_CONTEXT)
            eglDestroyContext(w.display, w.context);
         eglTerminate(w.display);
      }
      w.egl_surface = EGL_NO_SURFACE;
      w.context = EGL_NO_CONTEXT;
      w.display = EGL_NO_DISPLAY;
      w.config = nullptr;
   }

   // OpenGL ES 3 on the Surface, and Skia on that. False (and logged) if any
   // of it fails; the view then stays undrawn until the next Surface.
   bool make_gpu(an::window& w)
   {
      w.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      if (w.display == EGL_NO_DISPLAY || !eglInitialize(w.display, nullptr, nullptr))
      {
         log_error("eglInitialize failed: 0x%x", eglGetError());
         w.display = EGL_NO_DISPLAY;
         return false;
      }

      EGLint const config_attribs[] = {
         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
         EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
         EGL_STENCIL_SIZE, 8,
         EGL_NONE
      };
      EGLint count = 0;
      if (!eglChooseConfig(w.display, config_attribs, &w.config, 1, &count) || count < 1)
      {
         log_error("eglChooseConfig found no RGBA8888 ES3 config: 0x%x", eglGetError());
         release_gpu(w);
         return false;
      }

      EGLint const context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
      w.context = eglCreateContext(w.display, w.config, EGL_NO_CONTEXT, context_attribs);
      if (w.context == EGL_NO_CONTEXT)
      {
         log_error("eglCreateContext failed: 0x%x", eglGetError());
         release_gpu(w);
         return false;
      }

      w.egl_surface = eglCreateWindowSurface(w.display, w.config, w.surface, nullptr);
      if (w.egl_surface == EGL_NO_SURFACE)
      {
         log_error("eglCreateWindowSurface failed: 0x%x", eglGetError());
         release_gpu(w);
         return false;
      }

      if (!eglMakeCurrent(w.display, w.egl_surface, w.egl_surface, w.context))
      {
         log_error("eglMakeCurrent failed: 0x%x", eglGetError());
         release_gpu(w);
         return false;
      }
      // Presented once a display frame, from Choreographer: no need to wait
      // on the swap as well.
      eglSwapInterval(w.display, 0);

      auto gl = GrGLInterfaces::MakeEGL();
      w.gr = gl ? GrDirectContexts::MakeGL(gl) : nullptr;
      if (!w.gr)
      {
         log_error("GrDirectContexts::MakeGL failed");
         release_gpu(w);
         return false;
      }
      log_info("GL renderer: %s, %s", (char const*) glGetString(GL_RENDERER), (char const*) glGetString(GL_VERSION));
      return true;
   }

   // ── Drawing ──────────────────────────────────────────────────────────────────

   void draw_back(an::window& w, ph::host_view& h, ph::rect area)
   {
      SkCanvas* sk = w.back->getCanvas();
      sk->save();
      sk->scale(h.scale, h.scale);
      sk->clipRect(SkRect::MakeLTRB(area.left, area.top, area.right, area.bottom));
      sk->drawColor(SK_ColorBLACK, SkBlendMode::kSrc);
      {
         auto cnv = canvas{sk};
         h.owner->draw(cnv);
      }
      sk->restore();
   }

   ph::rect unite(ph::rect a, ph::rect const& b)
   {
      return {std::min(a.left, b.left), std::min(a.top, b.top),
              std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
   }

   // When something was invalidated: the areas are drawn, each apart, into
   // the surface that keeps the window, which is then copied to the Surface
   // and presented. The whole view, a resize or more than a few areas (their
   // union) draws it all.
   void render(an::window& w, ph::host_view& h)
   {
      h.needs_render = false;
      if (!w.gr || !h.opened || w.width <= 0 || w.height <= 0)
         return;
      if (!eglMakeCurrent(w.display, w.egl_surface, w.egl_surface, w.context))
      {
         log_error("eglMakeCurrent failed: 0x%x", eglGetError());
         return;
      }

      if (!w.front || w.front->width() != w.width || w.front->height() != w.height)
      {
         GrGLFramebufferInfo info;
         info.fFBOID = 0;
         info.fFormat = GL_RGBA8;
         auto target = GrBackendRenderTargets::MakeGL(w.width, w.height, 0, 8, info);
         w.front = SkSurfaces::WrapBackendRenderTarget(
            w.gr.get(), target, kBottomLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, nullptr, nullptr);
         w.back = SkSurfaces::RenderTarget(
            w.gr.get(), skgpu::Budgeted::kYes,
            SkImageInfo::Make(w.width, w.height, kRGBA_8888_SkColorType, kPremul_SkAlphaType),
            0, kTopLeft_GrSurfaceOrigin, nullptr);
         h.pending_full = true;
         if (!w.front || !w.back)
         {
            log_error("Skia could not make the window's surfaces (%dx%d)", w.width, w.height);
            w.front.reset();
            w.back.reset();
            return;
         }
      }

      auto rects = std::move(h.pending_rects);
      h.pending_rects.clear();
      ph::rect const all{0, 0, h.size.x, h.size.y};
      if (h.pending_full || rects.empty())
      {
         draw_back(w, h, all);
      }
      else if (rects.size() > 8)
      {
         auto u = rects.front();
         for (auto const& r : rects)
            u = unite(u, r);
         draw_back(w, h, u);
      }
      else
      {
         for (auto const& r : rects)
            draw_back(w, h, r);
      }
      h.pending_full = false;

      SkPaint copy;
      copy.setBlendMode(SkBlendMode::kSrc);
      w.front->getCanvas()->drawImage(w.back->makeImageSnapshot(), 0, 0, SkSamplingOptions{}, &copy);
      w.gr->flushAndSubmit(w.front.get(), GrSyncCpu::kNo);
      if (!eglSwapBuffers(w.display, w.egl_surface))
         log_error("eglSwapBuffers failed: 0x%x", eglGetError());
   }

   // The view's logical size follows the Surface's.
   void follow_surface(an::window& w, ph::host_view& h)
   {
      if (w.width <= 0 || w.height <= 0)
         return;
      auto const scale = scale_for(w);
      ph::extent const size{float(w.width) / scale, float(w.height) / scale};
      h.pending_full = true;
      h.needs_render = true;

      if (!h.opened)
      {
         h.scale = scale;
         h.size = size;
         h.opened = true;
         h.owner->on_open(size, scale);
         if (w.focused)
            h.owner->begin_focus();
         return;
      }
      if (scale != h.scale)
      {
         h.scale = scale;
         h.owner->on_scale_change(scale);
      }
      if (size.x != h.size.x || size.y != h.size.y)
      {
         h.size = size;
         h.owner->on_size_change(size);
      }
   }

   ph::point to_logical(ph::host_view const& h, float x, float y)
   {
      return {x / h.scale, y / h.scale};
   }

   void cancel_long_press(ph::host_view& h)
   {
      h.long_press_armed = false;
   }

   // A second finger turns a press into a scroll: let go of the first, away
   // from whatever it pressed.
   void begin_pan(ph::host_view& h, ph::point centre)
   {
      h.panning = true;
      h.pan_last = centre;
      if (h.touch_id && !h.long_press_fired)
      {
         cancel_long_press(h);
         h.long_press_fired = true;
         h.owner->click(button(ph::mouse_button::left, {far_away, far_away}, false, 1));
      }
   }

   ph::point centroid(ph::host_view const& h, int count, float const* xs, float const* ys)
   {
      float x = 0, y = 0;
      for (int i = 0; i < count; ++i)
      {
         x += xs[i];
         y += ys[i];
      }
      return to_logical(h, x / float(count), y / float(count));
   }
}

namespace cycfi::elements::android
{
   window* make_window(float density)
   {
      auto* w = new window;
      w->density = density;
      return w;
   }

   void destroy_window(window* w)
   {
      if (!w)
         return;
      if (w->view)
         w->view->window = nullptr;
      release_gpu(*w);
      if (w->surface)
         ANativeWindow_release(w->surface);
      delete w;
   }

   void surface_created(window* w, ANativeWindow* surface)
   {
      if (!w || !surface)
         return;
      if (w->surface != surface)
      {
         release_gpu(*w);
         if (w->surface)
            ANativeWindow_release(w->surface);
         ANativeWindow_acquire(surface);
         w->surface = surface;
      }
      w->width = ANativeWindow_getWidth(surface);
      w->height = ANativeWindow_getHeight(surface);
      if (!w->gr)
         make_gpu(*w);
      if (w->view)
         follow_surface(*w, *w->view);
   }

   void surface_changed(window* w, int width, int height)
   {
      if (!w)
         return;
      w->width = width;
      w->height = height;
      if (w->view)
         follow_surface(*w, *w->view);
   }

   void surface_destroyed(window* w)
   {
      if (!w)
         return;
      release_gpu(*w);
      if (w->surface)
         ANativeWindow_release(w->surface);
      w->surface = nullptr;
   }

   void logical_size(window const* w, uint32_t& width, uint32_t& height)
   {
      width = height = 0;
      if (!w || w->width <= 0 || w->height <= 0)
         return;
      auto const scale = scale_for(*w);
      width = uint32_t(std::lround(float(w->width) / scale));
      height = uint32_t(std::lround(float(w->height) / scale));
   }

   void frame(window* w, double now)
   {
      if (!w || !w->view)
         return;
      auto& h = *w->view;
      // Opened on the first frame with a Surface, after the view was made
      // (as the iOS host defers on_open past construction).
      if (!h.opened)
         follow_surface(*w, h);
      if (!h.opened)
         return;

      // The long press: a finger held still that long is a right click.
      if (h.long_press_armed && h.touch_id && !h.long_press_fired && now - h.touch_time >= long_press_seconds)
      {
         h.long_press_armed = false;
         h.long_press_fired = true;
         h.owner->click(button(ph::mouse_button::left, {far_away, far_away}, false, 1));
         h.owner->click(button(ph::mouse_button::right, h.cursor, true, 1));
         h.owner->click(button(ph::mouse_button::right, h.cursor, false, 1));
      }

      h.owner->poll();
      // poll may have destroyed the view (an editor closed from its own UI).
      if (w->view && w->view->needs_render)
         render(*w, *w->view);
   }

   void touch(window* w, int action, int pointer_id, int count,
              int const* ids, float const* xs, float const* ys, double now)
   {
      if (!w || !w->view || count <= 0)
         return;
      auto& h = *w->view;
      if (!h.opened)
         return;

      auto position_of = [&](int id) -> std::optional<ph::point>
      {
         for (int i = 0; i < count; ++i)
            if (ids[i] == id)
               return to_logical(h, xs[i], ys[i]);
         return std::nullopt;
      };

      switch (action)
      {
         case touch_down:
         {
            h.panning = false;
            h.touch_id = pointer_id;
            h.touch_start = h.cursor = position_of(pointer_id).value_or(h.cursor);
            h.touch_time = now;
            h.long_press_fired = false;
            h.long_press_armed = true;

            bool const again = h.last_tap_time >= 0 && now - h.last_tap_time <= double_tap_seconds
               && std::hypot(h.cursor.x - h.last_tap_at.x, h.cursor.y - h.last_tap_at.y) <= double_tap_slop;
            h.tap_count = again ? h.tap_count + 1 : 1;

            h.owner->cursor(h.cursor, ph::cursor_tracking::entering);
            h.owner->click(button(ph::mouse_button::left, h.cursor, true, h.tap_count));
            break;
         }

         case touch_pointer_down:
            if (count >= 2 && !h.panning)
               begin_pan(h, centroid(h, count, xs, ys));
            break;

         case touch_move:
            if (h.panning)
            {
               if (count >= 2)
               {
                  auto const c = centroid(h, count, xs, ys);
                  auto const dx = c.x - h.pan_last.x;
                  auto const dy = c.y - h.pan_last.y;
                  h.pan_last = c;
                  if (dx != 0 || dy != 0)
                     h.owner->scroll({dx, dy}, c);
               }
               break;
            }
            if (!h.touch_id || h.long_press_fired)
               break;
            if (auto p = position_of(*h.touch_id))
            {
               h.cursor = *p;
               if (std::hypot(h.cursor.x - h.touch_start.x, h.cursor.y - h.touch_start.y) > long_press_slop)
                  cancel_long_press(h);
               h.owner->cursor(h.cursor, ph::cursor_tracking::hovering);
               h.owner->drag(button(ph::mouse_button::left, h.cursor, true, h.tap_count));
            }
            break;

         case touch_pointer_up:
            // One of two panning fingers lifted: the pan's centre jumps, so
            // it starts again from where the rest are.
            if (h.panning)
            {
               // The lifted pointer is still in this event; leave it out.
               std::vector<float> rx, ry;
               for (int i = 0; i < count; ++i)
                  if (ids[i] != pointer_id)
                  {
                     rx.push_back(xs[i]);
                     ry.push_back(ys[i]);
                  }
               if (!rx.empty())
                  h.pan_last = centroid(h, int(rx.size()), rx.data(), ry.data());
            }
            break;

         case touch_up:
         case touch_cancel:
         {
            bool const was_mouse = h.touch_id.has_value();
            cancel_long_press(h);
            auto const fired = h.long_press_fired;
            h.touch_id.reset();
            h.panning = false;
            if (!was_mouse || fired)
               break;
            if (auto p = position_of(pointer_id))
               h.cursor = *p;
            // A cancelled touch (a system gesture) still lets go of the
            // button, or the control under it would think it held for ever;
            // released far away, so it does nothing.
            auto const at = action == touch_cancel ? ph::point{far_away, far_away} : h.cursor;
            h.owner->click(button(ph::mouse_button::left, at, false, h.tap_count));
            h.owner->cursor(h.cursor, ph::cursor_tracking::leaving);
            if (action == touch_up)
            {
               h.last_tap_time = now;
               h.last_tap_at = h.cursor;
            }
            break;
         }

         default:
            break;
      }
   }

   void wheel(window* w, float dx, float dy, float x, float y)
   {
      if (!w || !w->view || !w->view->opened)
         return;
      auto& h = *w->view;
      // AXIS_VSCROLL is +1 a notch away from the user: content moves down.
      // A notch is about three lines of a desktop wheel.
      constexpr float notch = 30;
      h.owner->scroll({dx * notch, dy * notch}, to_logical(h, x, y));
   }

   bool key(window* w, int key_code, bool down, int meta, int repeat, uint32_t codepoint)
   {
      if (!w || !w->view || !w->view->opened)
         return false;
      auto& h = *w->view;
      auto const code = translate_key(key_code);
      auto const mods = translate_meta(meta);
      auto const action = down ? (repeat > 0 ? ph::key_action::repeat : ph::key_action::press)
                               : ph::key_action::release;
      bool handled = false;
      if (code != ph::key_code::unknown)
         handled = handle_key(h.keys, *h.owner, {code, action, mods});

      if (down && !handled && codepoint >= 32 && !(codepoint >= 127 && codepoint < 160)
         && !(mods & (ph::mod_control | ph::mod_super)))
         handled = h.owner->text({codepoint, mods});
      return handled;
   }

   void focus(window* w, bool focused)
   {
      if (!w)
         return;
      w->focused = focused;
      if (!w->view || !w->view->opened)
         return;
      if (focused)
         w->view->owner->begin_focus();
      else
         w->view->owner->end_focus();
   }
}

///////////////////////////////////////////////////////////////////////////////
// base_view
namespace cycfi::elements
{
   namespace
   {
      host_view* make_host(base_view* owner, android::window* w, extent size)
      {
         init_resources();
         auto* h = new host_view;
         h->owner = owner;
         h->size = size;
         h->window = w;
         if (w)
         {
            if (w->view)
               w->view->window = nullptr;
            w->view = h;
         }
         return h;
      }
   }

   // A view with no window: it has a size and draws nowhere. QPlug makes the
   // view in the window it is given (unparented_view_ok is false).
   base_view::base_view(extent size_)
   {
      _view = make_host(this, nullptr, size_);
   }

   base_view::base_view(host_window_handle h)
   {
      _view = make_host(this, reinterpret_cast<android::window*>(h), {0, 0});
   }

   // The parent is the activity's android::window (android_host.hpp). The
   // view opens on the next frame (android::frame).
   base_view::base_view(host_view_handle h)
   {
      _view = make_host(this, reinterpret_cast<android::window*>(h), {0, 0});
   }

   base_view::~base_view()
   {
      auto* h = _view;
      if (h)
      {
         if (h->window && h->window->view == h)
            h->window->view = nullptr;
         delete h;
      }
      _view = nullptr;
   }

   point base_view::cursor_pos() const
   {
      return host()->cursor;
   }

   extent base_view::size() const
   {
      return host()->size;
   }

   // The Surface decides the size: the app gives the plugin its logical size
   // (android::logical_size), and a size asked for here is taken only before
   // there is a window to follow.
   void base_view::size(elements::extent size_)
   {
      auto* h = host();
      if (!h->window || h->window->width <= 0)
         h->size = size_;
   }

   void base_view::refresh()
   {
      auto* h = host();
      h->pending_full = true;
      h->pending_rects.clear();
      h->needs_render = true;
   }

   void base_view::refresh(rect area)
   {
      auto* h = host();
      if (!h->pending_full)
      {
         // A pixel round it for antialiased edges, and areas that touch
         // become one.
         area = {std::floor(area.left - 1), std::floor(area.top - 1),
                 std::ceil(area.right + 1), std::ceil(area.bottom + 1)};
         bool merged = false;
         for (auto& r : h->pending_rects)
         {
            rect const near{r.left - 4, r.top - 4, r.right + 4, r.bottom + 4};
            if (intersects(near, area))
            {
               r = unite(r, area);
               merged = true;
               break;
            }
         }
         if (!merged)
            h->pending_rects.push_back(area);
      }
      h->needs_render = true;
   }

   // Nothing is dropped on the view, so the types are only kept.
   void base_view::accept_drop_types(mime_types const& types)
   {
      _drop_types = types;
   }

   // Text is typed on Nano Q's own on-screen keyboard or a hardware one, with
   // no composition drawn by the system, so there is no caret to place.
   void base_view::text_caret(rect /* bounds */)
   {
   }

   // The clipboard is the process's own: Android's ClipboardManager is a
   // Java service, not reached from here yet.
   namespace
   {
      payload& the_clipboard()
      {
         static payload clip;
         return clip;
      }
   }

   std::vector<std::string> clipboard_types()
   {
      std::vector<std::string> types;
      for (auto const& [type, data] : the_clipboard())
         types.push_back(type);
      return types;
   }

   bool clipboard_contains(std::string_view mime_type)
   {
      return the_clipboard().count(std::string{mime_type}) != 0;
   }

   payload get_clipboard(mime_types const& types)
   {
      payload data;
      for (auto const& [type, value] : the_clipboard())
         if (types.count(type))
            data[type] = value;
      return data;
   }

   void set_clipboard(payload const& data)
   {
      the_clipboard() = data;
   }

   // No cursor to set on a touch screen.
   void set_cursor(cursor_type /* type */)
   {
   }

   // Touch scrolling follows the fingers: natural.
   point scroll_direction()
   {
      return {+1.0f, +1.0f};
   }
}
