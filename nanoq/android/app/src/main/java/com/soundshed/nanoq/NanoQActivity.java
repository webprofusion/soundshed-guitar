package com.soundshed.nanoq;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Choreographer;
import android.view.DisplayCutout;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.FrameLayout;

import com.soundshed.audio.UsbAudioPath;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Soundshed Nano Q for Android: one SurfaceView that Nano Q's editor draws into (libnanoq.so,
 * Elements' Android host), with the plugin played through Oboe.
 *
 * The status bar and the navigation bar (or a tablet's taskbar) stay on screen, and the surface
 * sits inside them, as the iOS app's editor sits inside its safe area: the window is laid out edge
 * to edge (targetSdk 35 and later enforce that anyway) and the surface's parent is padded by the
 * system-bar and display-cutout insets, which are followed as they change.
 *
 * Everything native is driven from this activity's main thread: surface and input events as they
 * arrive, and a Choreographer frame callback that runs the plugin's main-thread work and draws.
 * See docs/plans/nano-q-android.md.
 */
public class NanoQActivity extends Activity
        implements SurfaceHolder.Callback, Choreographer.FrameCallback {

    private static final String TAG = "NanoQ";
    private static final int AUDIO_PERMISSION_REQUEST = 1;

    static {
        System.loadLibrary("nanoq");
    }

    private static native long nativeCreate(String libraryPath, String resourceRoot,
                                            String profileDir, String homeDir, String statePath,
                                            float density);
    private static native void nativeDestroy(long handle);
    private static native void nativeSurfaceCreated(long handle, Object surface);
    private static native void nativeSurfaceChanged(long handle, int width, int height);
    private static native void nativeSurfaceDestroyed(long handle);
    private static native void nativeFrame(long handle, long frameTimeNanos);
    private static native void nativeTouch(long handle, int action, int pointerId, int[] ids,
                                           float[] xs, float[] ys, long eventTimeMillis);
    private static native void nativeWheel(long handle, float dx, float dy, float x, float y);
    private static native boolean nativeKey(long handle, int keyCode, boolean down, int meta,
                                            int repeat, int codepoint);
    private static native void nativeFocus(long handle, boolean focused);
    private static native boolean nativeStartAudio(long handle, boolean withInput);
    private static native void nativeStopAudio(long handle);
    private static native void nativeSaveState(long handle);

    private long handle;
    private FrameLayout root;
    private SurfaceView surfaceView;
    private boolean resumed;
    private boolean frameScheduled;
    private boolean audioStarted;
    private UsbAudioPath usbAudioPath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            window.getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        File resources = new File(getFilesDir(), "resources");
        unpackAssets(resources);
        File profile = new File(getFilesDir(), "profile");
        //noinspection ResultOfMethodCallIgnored
        profile.mkdirs();

        String library = getApplicationInfo().nativeLibraryDir + "/libnanoq.so";
        // libnanoq.so is loaded from the APK (useLegacyPackaging = false), so there is no file at
        // nativeLibraryDir; the host reaches the plugin's entry point linked in, by name only.
        handle = nativeCreate(library, resources.getAbsolutePath(), profile.getAbsolutePath(),
                getFilesDir().getAbsolutePath(),
                new File(getFilesDir(), "plugin-state.bin").getAbsolutePath(),
                getResources().getDisplayMetrics().density);
        // Before audio opens: whether a USB interface rules out AAudio's MMAP path.
        usbAudioPath = new UsbAudioPath(this);
        usbAudioPath.start();

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        surfaceView.setFocusable(true);
        surfaceView.setFocusableInTouchMode(true);
        // Touches on the view rather than the activity: with the surface inset, the editor wants
        // coordinates relative to the surface, not the window.
        surfaceView.setOnTouchListener((v, e) -> onSurfaceTouch(e));
        surfaceView.setOnGenericMotionListener((v, e) -> onGenericMotion(e));

        root = new FrameLayout(this);
        root.setBackgroundColor(getColor(R.color.nanoq_background));
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            applySafeArea(insets);
            return insets;
        });
        setContentView(root);
        layOutEdgeToEdge();
        surfaceView.requestFocus();
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            startAudio(true);
        } else {
            requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO},
                    AUDIO_PERMISSION_REQUEST);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != AUDIO_PERMISSION_REQUEST) return;
        boolean granted = grantResults.length > 0
                && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        if (!granted) Log.w(TAG, "Microphone permission denied: output only");
        startAudio(granted);
    }

    private void startAudio(boolean withInput) {
        if (audioStarted || handle == 0) return;
        audioStarted = nativeStartAudio(handle, withInput);
        if (!audioStarted) Log.w(TAG, "Audio did not start");
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        scheduleFrame();
    }

    @Override
    protected void onPause() {
        resumed = false;
        if (handle != 0) nativeSaveState(handle);
        super.onPause();
    }

    @Override
    protected void onStop() {
        if (handle != 0 && audioStarted) {
            nativeStopAudio(handle);
            audioStarted = false;
        }
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (usbAudioPath != null) usbAudioPath.stop();
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (handle != 0) nativeFocus(handle, hasFocus);
    }

    // --- Frames ---

    private void scheduleFrame() {
        if (frameScheduled || !resumed) return;
        frameScheduled = true;
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        frameScheduled = false;
        if (handle != 0) nativeFrame(handle, frameTimeNanos);
        scheduleFrame();
    }

    // --- Surface ---

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (handle != 0) nativeSurfaceCreated(handle, holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (handle != 0) nativeSurfaceChanged(handle, width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (handle != 0) nativeSurfaceDestroyed(handle);
    }

    // --- Input ---

    private boolean onSurfaceTouch(MotionEvent e) {
        if (handle == 0) return false;
        int count = e.getPointerCount();
        int[] ids = new int[count];
        float[] xs = new float[count];
        float[] ys = new float[count];
        for (int i = 0; i < count; ++i) {
            ids[i] = e.getPointerId(i);
            xs[i] = e.getX(i);
            ys[i] = e.getY(i);
        }
        int action = e.getActionMasked();
        int pointerId = e.getPointerId(e.getActionIndex());
        nativeTouch(handle, action, pointerId, ids, xs, ys, e.getEventTime());
        return true;
    }

    private boolean onGenericMotion(MotionEvent e) {
        if (handle == 0) return false;
        if (e.isFromSource(InputDevice.SOURCE_CLASS_POINTER)
                && e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            nativeWheel(handle, e.getAxisValue(MotionEvent.AXIS_HSCROLL),
                    e.getAxisValue(MotionEvent.AXIS_VSCROLL), e.getX(), e.getY());
            return true;
        }
        return false;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        // Volume and media keys stay with the system.
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN
                || code == KeyEvent.KEYCODE_VOLUME_MUTE || KeyEvent.isMediaSessionKey(code)) {
            return super.dispatchKeyEvent(e);
        }
        int action = e.getAction();
        if (handle != 0 && (action == KeyEvent.ACTION_DOWN || action == KeyEvent.ACTION_UP)) {
            boolean down = action == KeyEvent.ACTION_DOWN;
            if (code == KeyEvent.KEYCODE_BACK) {
                // Back closes whatever Nano Q has open (as Escape does); with nothing open it
                // sends the app to the background, keeping the sound and the editor as they are.
                boolean handled = nativeKey(handle, KeyEvent.KEYCODE_ESCAPE, down, 0, 0, 0);
                if (!handled && !down) moveTaskToBack(true);
                return true;
            }
            int codepoint = e.getUnicodeChar(e.getMetaState());
            if (nativeKey(handle, code, down, e.getMetaState(), e.getRepeatCount(), codepoint)) {
                return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    // --- Window ---

    /**
     * Lays the window out behind the system bars, so the insets arrive at {@link #root} for it to
     * pad by. From API 35 the platform does this whatever the app asks; below it, ask.
     */
    private void layOutEdgeToEdge() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false);
        } else {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
        root.requestApplyInsets();
    }

    /**
     * Pads the surface clear of the status bar, the navigation bar or taskbar, and a notch or
     * punch-hole. The surface resizes with the padding, so the editor hears of it through
     * surfaceChanged, as it does for a rotation.
     */
    private void applySafeArea(WindowInsets insets) {
        int left;
        int top;
        int right;
        int bottom;
        if (Build.VERSION.SDK_INT >= 30) {
            Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            left = bars.left;
            top = bars.top;
            right = bars.right;
            bottom = bars.bottom;
        } else {
            left = insets.getSystemWindowInsetLeft();
            top = insets.getSystemWindowInsetTop();
            right = insets.getSystemWindowInsetRight();
            bottom = insets.getSystemWindowInsetBottom();
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                left = Math.max(left, cutout.getSafeInsetLeft());
                top = Math.max(top, cutout.getSafeInsetTop());
                right = Math.max(right, cutout.getSafeInsetRight());
                bottom = Math.max(bottom, cutout.getSafeInsetBottom());
            }
        }
        if (root.getPaddingLeft() == left && root.getPaddingTop() == top
                && root.getPaddingRight() == right && root.getPaddingBottom() == bottom) {
            return;
        }
        Log.i(TAG, "Safe-area insets (px): left=" + left + " top=" + top + " right=" + right
                + " bottom=" + bottom);
        root.setPadding(left, top, right, bottom);
    }

    // --- Assets ---

    /**
     * Copies assets/nanoq (fonts, factory presets, engine data) to files/resources once per
     * install or update: Nano Q reads them as files.
     */
    private void unpackAssets(File target) {
        String stamp;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            stamp = Long.toString(info.lastUpdateTime);
        } catch (PackageManager.NameNotFoundException e) {
            stamp = "unknown";
        }
        File stampFile = new File(target, ".unpacked");
        try {
            if (stampFile.isFile() && stamp.equals(new String(
                    Files.readAllBytes(stampFile.toPath()), StandardCharsets.UTF_8))) {
                return;
            }
        } catch (IOException ignored) {
            // Unpack again.
        }

        long started = System.nanoTime();
        deleteTree(target);
        int[] files = {0};
        try {
            copyAssetTree(getAssets(), "nanoq", target, files);
            try (OutputStream out = new FileOutputStream(stampFile)) {
                out.write(stamp.getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException e) {
            Log.e(TAG, "Unpacking resources failed", e);
        }
        Log.i(TAG, "Unpacked " + files[0] + " resource files in "
                + (System.nanoTime() - started) / 1000000 + " ms");
    }

    private static void copyAssetTree(AssetManager assets, String path, File target, int[] files)
            throws IOException {
        String[] children = assets.list(path);
        if (children != null && children.length > 0) {
            //noinspection ResultOfMethodCallIgnored
            target.mkdirs();
            for (String child : children) {
                copyAssetTree(assets, path + "/" + child, new File(target, child), files);
            }
            return;
        }
        // A file (or an empty directory, which assets do not keep).
        try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[64 * 1024];
            int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
        }
        ++files[0];
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
