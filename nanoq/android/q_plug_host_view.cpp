// QPlug's platform view glue for Android, as nanoq/ios/q_plug_host_view.mm is for iOS.
//
// QPlug asks for CLAP_WINDOW_API_X11 on every platform that is neither Windows nor Apple, and
// takes the parent from clap_window_t's x11 field. On Android the app passes the activity's
// cycfi::elements::android::window there (nanoq/android/elements_host/android_host.hpp): an X11
// window id is an unsigned long, which holds a pointer on arm64 and x86_64. See
// docs/plans/nano-q-android.md.

#include <q_plug/host_view.hpp>

namespace cycfi::q_plug::detail
{
// The view lives in the activity's window, so it waits for the parent to arrive in attach, as
// on Windows and Linux.
extern bool const unparented_view_ok = false;

plugin_view* make_view(void* parent, elements::extent)
{
    return new plugin_view(static_cast<elements::host_view_handle>(parent));
}

// Nothing to do: the view was made in the window it was given.
void add_subview(void*, void*) {}

// The app deals in the view's logical units, as an iOS app deals in points.
float pixel_scale(plugin_view const*)
{
    return 1.0f;
}

// Events arrive from the activity, and the app polls the view once a display frame.
int event_fd()
{
    return -1;
}

void pump_events() {}
} // namespace cycfi::q_plug::detail
