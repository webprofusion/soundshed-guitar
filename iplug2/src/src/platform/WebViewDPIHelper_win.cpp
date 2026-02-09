#ifdef _WIN32

#include "WebViewDPIHelper_win.h"
#include <windows.h>
#include <iostream>

namespace guitarfx {

bool SetWebViewDPIScale(void* parentWindow)
{
  if (!parentWindow)
  {
    std::cerr << "[WebViewDPI] No parent window provided" << std::endl;
    return false;
  }

  HWND hwnd = static_cast<HWND>(parentWindow);

  // Get the DPI for this window
  UINT dpi = GetDpiForWindow(hwnd);
  const double scale = static_cast<double>(dpi) / 96.0; // 96 is the standard DPI (100%)
  
  std::cout << "[WebViewDPI] Window DPI: " << dpi << " (" << (scale * 100.0) << "% scale)" << std::endl;

  // Check DPI awareness mode
  DPI_AWARENESS_CONTEXT dpiContext = GetWindowDpiAwarenessContext(hwnd);
  
  if (AreDpiAwarenessContextsEqual(dpiContext, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
  {
    std::cout << "[WebViewDPI] ✓ Window is Per-Monitor V2 DPI aware" << std::endl;
  }
  else if (AreDpiAwarenessContextsEqual(dpiContext, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
  {
    std::cout << "[WebViewDPI] ⚠ Window is Per-Monitor V1 DPI aware (V2 preferred)" << std::endl;
  }
  else if (AreDpiAwarenessContextsEqual(dpiContext, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
  {
    std::cout << "[WebViewDPI] ⚠ Window is System DPI aware (Per-Monitor V2 recommended)" << std::endl;
  }
  else
  {
    std::cout << "[WebViewDPI] ⚠ Window is DPI unaware (manifest may not be embedded)" << std::endl;
  }

  // With Per-Monitor V2 DPI awareness enabled in the manifest:
  // - Windows informs the application and WebView2 of DPI changes
  // - WebView2's ShouldDetectMonitorScaleChanges property defaults to TRUE
  // - WebView2 automatically adjusts RasterizationScale when DPI changes
  // - Content should render crisply at the native resolution
  //
  // Technical Details:
  // - ICoreWebView2Controller3::put_ShouldDetectMonitorScaleChanges(TRUE) enables auto-scaling
  // - This property defaults to TRUE, so explicit setting is usually unnecessary
  // - The iPlug2 WebView wrapper doesn't expose ICoreWebView2Controller3, but the
  //   default behavior handles DPI changes automatically when the app is DPI-aware
  //
  // Automatic behavior sequence:
  // 1. App manifest declares Per-Monitor V2 DPI awareness
  // 2. Windows notifies WebView2 of DPI/scale changes
  // 3. WebView2 (with ShouldDetectMonitorScaleChanges=TRUE) updates RasterizationScale
  // 4. Content re-renders at the correct pixel density

  std::cout << "[WebViewDPI] WebView2 will automatically use RasterizationScale=" << scale << std::endl;
  std::cout << "[WebViewDPI] Note: ShouldDetectMonitorScaleChanges defaults to TRUE in WebView2" << std::endl;
  std::cout << "[WebViewDPI]       This means WebView2 auto-adjusts when moving between displays" << std::endl;
  std::cout << "[WebViewDPI]" << std::endl;
  std::cout << "[WebViewDPI] If content appears blurry:" << std::endl;
  std::cout << "[WebViewDPI]   1. Verify app.manifest is embedded (check exe properties)" << std::endl;
  std::cout << "[WebViewDPI]   2. Ensure dpiAwareness=PerMonitorV2 in manifest" << std::endl;
  std::cout << "[WebViewDPI]   3. Restart the application/host after rebuilding" << std::endl;
  std::cout << "[WebViewDPI]   4. Check if explicit put_ShouldDetectMonitorScaleChanges(TRUE) is needed" << std::endl;

  return true;
}

} // namespace guitarfx

#endif // _WIN32
