#pragma once

#ifdef _WIN32

namespace guitarfx {

/**
 * @brief Checks and reports DPI scaling information for a WebView2 control.
 * 
 * This function queries the system DPI and DPI awareness context to verify
 * that the application is properly configured for high-DPI displays. With
 * Per-Monitor V2 DPI awareness enabled (via app.manifest), WebView2 will
 * automatically apply the correct RasterizationScale for crisp rendering.
 * 
 * WebView2's ICoreWebView2Controller3::ShouldDetectMonitorScaleChanges property
 * defaults to TRUE, which enables automatic RasterizationScale adjustments when
 * the window moves between monitors with different DPIs. This is the recommended
 * behavior for most applications.
 * 
 * The function logs diagnostic information including:
 * - Current window DPI and scale factor
 * - DPI awareness mode (Per-Monitor V2, V1, System, or Unaware)
 * - Expected RasterizationScale that WebView2 should use
 * - Troubleshooting tips if content appears blurry
 * 
 * Note: This is a diagnostic/informational function. WebView2's RasterizationScale
 * is set automatically by the WebView2 runtime based on the app's DPI awareness.
 * Manual control would require accessing ICoreWebView2Controller3, which is not
 * exposed by iPlug2's WebView wrapper.
 * 
 * @param parentWindow The parent window handle (HWND) containing the WebView
 * @return true if DPI information was successfully retrieved, false otherwise
 */
bool SetWebViewDPIScale(void* parentWindow);

} // namespace guitarfx

#endif // _WIN32
