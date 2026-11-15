package com.soundshed.audio;

import android.content.Context;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.Arrays;

/**
 * Keeps AAudio off its MMAP path while a USB audio interface with no 16-bit format is attached.
 *
 * <p>On a Galaxy S21 the MMAP path opens USB interfaces at 16 bits whatever they offer, and one
 * without a 16-bit format records and plays full-scale noise; Android's normal path handles it.
 * This class watches devices come and go and tells the native half
 * ({@code juce/source/AndroidUsbAudioPath.cpp}), which sets the process's MMAP policy and has the
 * app reopen its streams when the answer changes.
 *
 * <p>Shared by Soundshed Guitar's Android app and Nano Q's; both compile this directory. Main
 * thread only.
 */
public final class UsbAudioPath
{
    private static final String TAG = "SoundshedGuitar";

    private final AudioManager audio;
    private boolean started;

    private final AudioDeviceCallback callback = new AudioDeviceCallback()
    {
        @Override
        public void onAudioDevicesAdded (AudioDeviceInfo[] added)
        {
            update();
        }

        @Override
        public void onAudioDevicesRemoved (AudioDeviceInfo[] removed)
        {
            update();
        }
    };

    public UsbAudioPath (Context context)
    {
        audio = context.getApplicationContext().getSystemService (AudioManager.class);
    }

    /** Decides now, before the app opens audio, then follows devices until {@link #stop()}. */
    public void start()
    {
        if (started || audio == null)
            return;

        started = true;
        update();
        audio.registerAudioDeviceCallback (callback, new Handler (Looper.getMainLooper()));
    }

    public void stop()
    {
        if (! started)
            return;

        started = false;
        audio.unregisterAudioDeviceCallback (callback);
    }

    private void update()
    {
        String reason = null;

        for (AudioDeviceInfo device : audio.getDevices (AudioManager.GET_DEVICES_ALL))
        {
            final int type = device.getType();

            if (type != AudioDeviceInfo.TYPE_USB_DEVICE && type != AudioDeviceInfo.TYPE_USB_HEADSET)
                continue;

            // An empty list means the device takes any encoding.
            final int[] encodings = device.getEncodings();

            if (encodings.length == 0)
                continue;

            boolean has16Bit = false;

            for (int encoding : encodings)
                has16Bit |= encoding == AudioFormat.ENCODING_PCM_16BIT;

            if (! has16Bit)
            {
                reason = device.getProductName() + " " + (device.isSource() ? "input" : "output")
                       + " " + Arrays.toString (encodings);
                break;
            }
        }

        try
        {
            nativeSetMMapAllowed (reason == null, reason == null ? "" : reason);
        }
        catch (UnsatisfiedLinkError e)
        {
            Log.w (TAG, "USB audio: the native library is not loaded yet", e);
        }
    }

    private static native void nativeSetMMapAllowed (boolean allowed, String reason);
}
