package com.soundshed.guitar;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.View;

/**
 * The app's only activity.
 *
 * <p>JUCE creates and owns the content view; this class just has to exist and
 * be a plain {@link Activity}. JUCE finds it through the
 * {@code Application.ActivityLifecycleCallbacks} it registers from
 * {@code Java.initialiseJUCE}, so no particular base class is required.
 *
 * <p>Note it deliberately does <em>not</em> extend {@code com.rmsl.juce.JuceActivity}.
 * That class declares {@code appNewIntent} and {@code appOnResume} as native
 * methods, but JUCE only compiles their implementations when
 * {@code JUCE_PUSH_NOTIFICATIONS_ACTIVITY} is defined — which is why it lives in
 * JUCE's optional {@code javaopt} directory. Extending it without push
 * notifications enabled makes the app die with an UnsatisfiedLinkError the
 * moment the activity resumes.
 *
 * <p>Safe-area insets are published from the WebView rather than from here — see
 * {@link SafeAreaInsets} for why this activity cannot reach it.
 */
public class MainActivity extends Activity
{
    private static final int REQUEST_RECORD_AUDIO = 1001;

    @Override
    protected void onCreate (Bundle savedInstanceState)
    {
        initEdgeToEdge();

        super.onCreate (savedInstanceState);

        // Without RECORD_AUDIO the audio device opens with no input and the amp
        // has nothing to process.
        if (checkSelfPermission (Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            requestPermissions (new String[] { Manifest.permission.RECORD_AUDIO }, REQUEST_RECORD_AUDIO);
    }

    /** Mirrors com.rmsl.juce.JuceActivity.initEdgeToEdge() for minSdk 29+. */
    private void initEdgeToEdge()
    {
        final View decorView = getWindow().getDecorView();

        if (Build.VERSION.SDK_INT < 30)
        {
            decorView.setSystemUiVisibility (decorView.getSystemUiVisibility()
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
        else
        {
            getWindow().setDecorFitsSystemWindows (false);
        }

        // Android 15 (35) handles bar contrast itself.
        if (Build.VERSION.SDK_INT < 35)
            getWindow().setStatusBarContrastEnforced (false);

        getWindow().setNavigationBarContrastEnforced (false);
    }
}
