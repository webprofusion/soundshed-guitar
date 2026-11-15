#pragma once

/**
 * AndroidUsbAudioPath — keeps AAudio off its MMAP path for USB interfaces that path mangles.
 *
 * AAudio's low-latency streams run on an MMAP path where the device has one. On a Galaxy S21
 * (Android 15) that path opens a USB interface at 16 bits whatever the interface offers, and one
 * with no 16-bit format then records and plays full-scale noise: the POGO, whose only format is
 * 24-bit samples in 4-byte slots, did exactly that (2026-11-15). Android's normal path handles the
 * same interface cleanly, at about one more burst of latency each way.
 *
 * The rule: while a USB audio device with no 16-bit PCM encoding is attached, this process's
 * AAudio MMAP policy is NEVER; otherwise it is AUTO, the system default. The Java half,
 * com.soundshed.audio.UsbAudioPath (android/shared/java), watches devices come and go and reports
 * the rule's answer here. The policy only affects streams opened after it changes, so each app
 * registers a handler that reopens its streams when the answer changes.
 *
 * Shared by Soundshed Guitar's Android app and Nano Q's (no JUCE in it). What was decided goes to
 * logcat under the SoundshedGuitar tag. Everything here is a no-op outside Android.
 */

#include <functional>

namespace soundshed::android_audio
{
/** Main thread. The handler runs on the main thread when the MMAP rule changes, so the app can
    reopen its streams; an empty function removes it. */
void SetUsbAudioPathHandler(std::function<void()> handler);

/** False while a USB audio interface with no 16-bit format is attached. */
bool UsbMMapAllowed() noexcept;
} // namespace soundshed::android_audio
