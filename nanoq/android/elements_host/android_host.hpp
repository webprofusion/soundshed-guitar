/*=============================================================================
   Elements' host layer for Android: what the app hands it.

   Elements has no Android host of its own. Nano Q's (base_view.cpp, beside
   this) draws with Artist's Skia backend on OpenGL ES, into the Surface of
   the activity's SurfaceView, and takes its input from the activity. All of
   it runs on the app's main (UI) thread, which is the thread every function
   here is called on.

   The app makes one `window` for the activity and passes it to the plugin as
   its parent (clap.gui's set_parent, as an X11 window id: QPlug asks for X11
   on every platform that is not Windows or Apple, and the id is only a
   number). The view the plugin makes in it then fills it.

   Sizes and positions the app passes are in the Surface's pixels. The view
   works in logical units of `scale` pixels each, which `window` decides from
   the screen's density and the Surface's height (see scale_for).

   See docs/plans/nano-q-android.md.

   Distributed under the MIT License (https://opensource.org/licenses/MIT),
   as Elements is.
=============================================================================*/
#if !defined(NANOQ_ANDROID_HOST_HPP)
#define NANOQ_ANDROID_HOST_HPP

#include <cstdint>

struct ANativeWindow;

namespace cycfi::elements::android
{
   struct window;

   /// The activity's window: made once the activity exists, destroyed with it.
   /// `density` is the display's (DisplayMetrics.density: pixels per dp).
   window*  make_window(float density);
   void     destroy_window(window* w);

   /// The SurfaceView's Surface: given when it is made or changes size, taken
   /// away when it is destroyed (the app went to the background). Drawing
   /// stops in between; the window is drawn whole when a Surface comes back.
   void     surface_created(window* w, ANativeWindow* surface);
   void     surface_changed(window* w, int width, int height);
   void     surface_destroyed(window* w);

   /// The logical size the view has (or will have) in the Surface, for the
   /// app to give the plugin's editor. Zero before there is a Surface.
   void     logical_size(window const* w, uint32_t& width, uint32_t& height);

   /// Once a display frame (Choreographer): Elements' poll (posted work and
   /// timers), the long press, and drawing what was invalidated.
   void     frame(window* w, double now_seconds);

   /// MotionEvent, flattened: the masked action, the id of the pointer it is
   /// about, and every pointer's id and position, in pixels.
   enum touch_action
   {
      touch_down = 0,            // ACTION_DOWN
      touch_up = 1,              // ACTION_UP
      touch_move = 2,            // ACTION_MOVE
      touch_cancel = 3,          // ACTION_CANCEL
      touch_pointer_down = 5,    // ACTION_POINTER_DOWN
      touch_pointer_up = 6       // ACTION_POINTER_UP
   };
   void     touch(window* w, int action, int pointer_id, int count,
               int const* ids, float const* xs, float const* ys, double now_seconds);

   /// A mouse wheel or touchpad scroll (ACTION_SCROLL), in pixels, at a place
   /// in pixels.
   void     wheel(window* w, float dx, float dy, float x, float y);

   /// A hardware key (KeyEvent): its key code and meta state, and the
   /// character it types (0 for none). True if the view took it.
   bool     key(window* w, int key_code, bool down, int meta, int repeat, uint32_t codepoint);

   /// The activity was paused or resumed: focus follows.
   void     focus(window* w, bool focused);
}

#endif
