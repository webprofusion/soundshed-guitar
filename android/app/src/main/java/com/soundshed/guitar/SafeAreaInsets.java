package com.soundshed.guitar;

import android.graphics.Insets;
import android.os.Build;
import android.util.Log;
import android.view.WindowInsets;
import android.webkit.WebView;

import java.util.Locale;

/**
 * Publishes the window's safe-area insets into the web UI as CSS custom
 * properties.
 *
 * <p>The app draws edge to edge — from targetSdk 35 Android enforces it, so
 * opting out is not a reliable fix — which puts the status bar over the header
 * and the navigation bar over the footer. The footer is the one that actually
 * breaks: the nav bar does not merely obscure those controls, it takes their
 * taps.
 *
 * <p>The page cannot work this out for itself. {@code env(safe-area-inset-*)} is
 * a Chromium feature that only reports real values on platforms which feed
 * insets to the renderer, and Android WebView is not one of them; there it reads
 * as zero. So the values are read here and pushed in as {@code --ssg-safe-top} /
 * {@code -bottom} / {@code -left} / {@code -right}, which
 * {@code core/ui/css/variables.css} declares as {@code 0px} for every other
 * platform.
 *
 * <p><b>Why this is driven from the WebView rather than the Activity.</b> JUCE
 * does not add its peer view to the activity's content view — with no native
 * parent supplied it goes through {@code WindowManager.addView} as a window of
 * its own (see {@code juce_Windowing_android.cpp}). The WebView is therefore not
 * reachable by walking {@code getWindow().getDecorView()} from
 * {@link MainActivity}; the only thing holding both the insets and the page is
 * the WebView itself. It is called from the two places that matter: after a page
 * finishes loading (a navigation discards the inline styles, and JUCE always
 * navigates once after creating the view) and when the view is resized (rotation).
 */
public final class SafeAreaInsets
{
    private static final String TAG = "SoundshedGuitar";

    private SafeAreaInsets() {}

    /** Reads the current insets from {@code view} and applies them to its page. */
    public static void apply (WebView view)
    {
        if (view == null)
            return;

        final WindowInsets insets = view.getRootWindowInsets();

        if (insets == null)
            return; // not attached to a window yet

        final int top;
        final int bottom;
        final int left;
        final int right;

        if (Build.VERSION.SDK_INT >= 30)
        {
            // System bars plus display cutout: a notch or punch-hole is just as
            // much a no-draw region as the status bar is.
            final Insets bars = insets.getInsets (
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            top = bars.top;
            bottom = bars.bottom;
            left = bars.left;
            right = bars.right;
        }
        else
        {
            top = insets.getSystemWindowInsetTop();
            bottom = insets.getSystemWindowInsetBottom();
            left = insets.getSystemWindowInsetLeft();
            right = insets.getSystemWindowInsetRight();
        }

        final float density = view.getResources().getDisplayMetrics().density;

        // CSS pixels rather than physical: the page is laid out in CSS px, and
        // the WebView's devicePixelRatio is this same density.
        final String script =
                "(function(){var s=document.documentElement.style;"
                        + "s.setProperty('--ssg-safe-top','" + toCssPx (top, density) + "');"
                        + "s.setProperty('--ssg-safe-bottom','" + toCssPx (bottom, density) + "');"
                        + "s.setProperty('--ssg-safe-left','" + toCssPx (left, density) + "');"
                        + "s.setProperty('--ssg-safe-right','" + toCssPx (right, density) + "');"
                        + "document.documentElement.dataset.ssgSafeAreas='1';})();";

        Log.i (TAG, "Safe-area insets (px): top=" + top + " bottom=" + bottom
                + " left=" + left + " right=" + right + " density=" + density);

        view.evaluateJavascript (script, null);
    }

    private static String toCssPx (int physicalPx, float density)
    {
        if (density <= 0f)
            return physicalPx + "px";

        return String.format (Locale.US, "%.2fpx", physicalPx / density);
    }
}
