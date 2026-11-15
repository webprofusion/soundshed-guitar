#include "AudioIO.h"

#include "AppLog.h"
#include "ClapHost.h"

#include <oboe/OboeExtensions.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace nanoq::android
{
namespace
{
constexpr int32_t kChannels = 2;

int64_t NowNanos()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

uint32_t NextPowerOfTwo(uint32_t n)
{
    uint32_t p = 64;
    while (p < n && p < 4096)
        p <<= 1;
    return p;
}

void RaisePeak(std::atomic<float>& peak, float value)
{
    float seen = peak.load(std::memory_order_relaxed);
    while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed))
    {
    }
}
} // namespace

AudioIO::AudioIO(ClapHost& host)
    : mHost(host)
{
}

AudioIO::~AudioIO()
{
    Stop();
}

bool AudioIO::Start(bool withInput)
{
    if (mRunning)
        return true;
    mWithInput = withInput;
    mRestart = false;

    // Whether these streams may take AAudio's MMAP path is decided per process, by
    // AndroidUsbAudioPath (a USB interface with no 16-bit format rules it out); it calls Restart
    // when that changes.
    oboe::AudioStreamBuilder out;
    out.setDirection(oboe::Direction::Output)
        ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
        ->setSharingMode(oboe::SharingMode::Exclusive)
        ->setFormat(oboe::AudioFormat::Float)
        ->setChannelCount(kChannels)
        ->setUsage(oboe::Usage::Media)
        ->setContentType(oboe::ContentType::Music)
        ->setDataCallback(this)
        ->setErrorCallback(this);
    auto result = out.openStream(mOutput);
    if (result != oboe::Result::OK)
    {
        AppLog("Nano Q audio: the output did not open (%s)", oboe::convertToText(result));
        mOutput.reset();
        return false;
    }
    // Two bursts of buffering: the least that rides out a late callback.
    mOutput->setBufferSizeInFrames(mOutput->getFramesPerBurst() * 2);
    mSampleRate = mOutput->getSampleRate();

    mInputChannels = 0;
    if (withInput)
    {
        // At the output's rate, so the two streams run off one clock as far as the device
        // allows. Stereo with conversion allowed: a mono microphone comes as two equal
        // channels, and an interface's two inputs as they are.
        oboe::AudioStreamBuilder in;
        in.setDirection(oboe::Direction::Input)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(kChannels)
            ->setSampleRate(int32_t(mSampleRate))
            ->setInputPreset(oboe::InputPreset::Unprocessed)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setErrorCallback(this);
        result = in.openStream(mInput);
        if (result != oboe::Result::OK)
        {
            AppLog("Nano Q audio: the input did not open (%s); playing without it", oboe::convertToText(result));
            mInput.reset();
        }
        else
        {
            mInputChannels = mInput->getChannelCount();
            // The input's buffer as large as it will go: a full-duplex pair reads what has
            // arrived each callback, and a deep input buffer only holds more of it.
            mInput->setBufferSizeInFrames(mInput->getBufferCapacityInFrames());
        }
    }

    mBlock = NextPowerOfTwo(uint32_t(std::max(mOutput->getFramesPerBurst(), 1)));
    for (auto& channel : mIn)
        channel.assign(mBlock, 0.0f);
    for (auto& channel : mOut)
        channel.assign(mBlock, 0.0f);

    if (!mHost.Activate(mSampleRate, mBlock))
    {
        AppLog("Nano Q audio: the plugin did not activate at %.0f Hz, %u frames", mSampleRate, mBlock);
        CloseStreams();
        return false;
    }

    setSharedOutputStream(mOutput);
    if (mInput)
        setSharedInputStream(mInput);

    mPlacement.prepare(mSampleRate);
    mCallbacks = 0;
    mInPeak = 0.0f;
    mOutPeak = 0.0f;
    mBusyNanos = 0;
    mLoadFrames = 0;
    mWorstLoad = 0.0f;
    mLoadReportAt = -1;

    if (mInput)
        result = FullDuplexStream::start();
    else
        result = mOutput->requestStart();
    if (result != oboe::Result::OK)
    {
        AppLog("Nano Q audio: the streams did not start (%s)", oboe::convertToText(result));
        mHost.Deactivate();
        CloseStreams();
        return false;
    }

    mRunning = true;
    mStartedAt = -1;
    mChecked = false;
    AppLog("Nano Q audio: started at %.0f Hz, burst %d, block %u, output %s, input %s (%d ch)", mSampleRate,
           mOutput->getFramesPerBurst(), mBlock,
           mOutput->getSharingMode() == oboe::SharingMode::Exclusive ? "exclusive" : "shared",
           mInput ? (mInput->getSharingMode() == oboe::SharingMode::Exclusive ? "exclusive" : "shared") : "none",
           mInputChannels);
    AppLog("Nano Q audio: output device %d%s, input device %d%s", mOutput->getDeviceId(),
           oboe::OboeExtensions::isMMapUsed(mOutput.get()) ? " (MMAP)" : "", mInput ? mInput->getDeviceId() : -1,
           mInput && oboe::OboeExtensions::isMMapUsed(mInput.get()) ? " (MMAP)" : "");
    return true;
}

void AudioIO::Stop()
{
    if (mRunning)
    {
        if (mInput)
            FullDuplexStream::stop();
        else if (mOutput)
            mOutput->requestStop();
    }
    mRunning = false;
    CloseStreams();
    // The streams are closed, so no data callback can still be running.
    mHost.Deactivate();
    mPlacement.release();
}

void AudioIO::Restart()
{
    if (!mRunning)
        return;
    const bool withInput = mWithInput;
    Stop();
    Start(withInput);
}

void AudioIO::CloseStreams()
{
    if (mOutput)
        mOutput->close();
    if (mInput)
        mInput->close();
    mOutput.reset();
    mInput.reset();
}

void AudioIO::Poll(double now)
{
    if (mRestart.exchange(false))
    {
        AppLog("Nano Q audio: a stream was disconnected; reopening");
        const bool withInput = mWithInput;
        Stop();
        Start(withInput);
        return;
    }

    if (!mRunning)
        return;

    if (mLoadReportAt < 0)
        mLoadReportAt = now + 10.0;
    else if (now >= mLoadReportAt)
    {
        mLoadReportAt = now + 10.0;
        const uint64_t busy = mBusyNanos.exchange(0);
        const uint64_t frames = mLoadFrames.exchange(0);
        const float worst = mWorstLoad.exchange(0.0f);
        if (frames > 0 && mSampleRate > 0)
        {
            const double average = double(busy) * 1e-9 / (double(frames) / mSampleRate);
            AppLog("Nano Q audio: DSP load %.0f%% average, %.0f%% worst callback", average * 100.0, worst * 100.0);
        }
    }

    if (mChecked)
        return;
    if (mStartedAt < 0)
    {
        mStartedAt = now;
        return;
    }
    if (now - mStartedAt < 3.0)
        return;
    mChecked = true;
    AppLog("Nano Q audio: %llu callbacks in the first 3 s, plugin %s, input peak %.4f, output peak %.4f",
           (unsigned long long)mCallbacks.load(), mHost.IsActive() ? "active" : "inactive", mInPeak.load(),
           mOutPeak.load());
}

// With no input stream the output's callback comes here directly: FullDuplexStream's own
// would read from an input that is not there.
oboe::DataCallbackResult AudioIO::onAudioReady(oboe::AudioStream* stream, void* audioData, int32_t numFrames)
{
    if (!mInput)
        return onBothStreamsReady(nullptr, 0, audioData, numFrames);
    return FullDuplexStream::onAudioReady(stream, audioData, numFrames);
}

oboe::DataCallbackResult AudioIO::onBothStreamsReady(const void* inputData, int numInputFrames, void* outputData,
                                                     int numOutputFrames)
{
    mPlacement.beginBlock();
    const int64_t started = NowNanos();
    mCallbacks.fetch_add(1, std::memory_order_relaxed);
    const auto* in = static_cast<const float*>(inputData);
    auto* out = static_cast<float*>(outputData);
    const int inChannels = std::max(mInputChannels, 1);
    const bool hasInput = in != nullptr && mInputChannels > 0;

    float inPeak = 0.0f, outPeak = 0.0f;
    for (int done = 0; done < numOutputFrames;)
    {
        const int frames = std::min(numOutputFrames - done, int(mBlock));

        // The input for this block, deinterleaved; what has not arrived is silence.
        const float* ins[kChannels] = {nullptr, nullptr};
        if (hasInput)
        {
            const int available = std::clamp(numInputFrames - done, 0, frames);
            for (int ch = 0; ch < kChannels; ++ch)
            {
                const int from = std::min(ch, inChannels - 1);
                float* dst = mIn[ch].data();
                for (int i = 0; i < available; ++i)
                {
                    const float v = in[(done + i) * inChannels + from];
                    dst[i] = v;
                    inPeak = std::max(inPeak, std::fabs(v));
                }
                std::fill(dst + available, dst + frames, 0.0f);
                ins[ch] = dst;
            }
        }

        float* outs[kChannels] = {mOut[0].data(), mOut[1].data()};
        mHost.Process(hasInput ? ins : nullptr, outs, uint32_t(frames));

        for (int i = 0; i < frames; ++i)
        {
            for (int ch = 0; ch < kChannels; ++ch)
            {
                const float v = outs[ch][i];
                out[(done + i) * kChannels + ch] = v;
                outPeak = std::max(outPeak, std::fabs(v));
            }
        }
        done += frames;
    }

    RaisePeak(mInPeak, inPeak);
    RaisePeak(mOutPeak, outPeak);

    const int64_t busy = NowNanos() - started;
    mBusyNanos.fetch_add(uint64_t(busy), std::memory_order_relaxed);
    mLoadFrames.fetch_add(uint64_t(numOutputFrames), std::memory_order_relaxed);
    if (numOutputFrames > 0 && mSampleRate > 0)
        RaisePeak(mWorstLoad, float(double(busy) * 1e-9 * mSampleRate / numOutputFrames));
    mPlacement.endBlock(numOutputFrames);
    return oboe::DataCallbackResult::Continue;
}

// Oboe's own thread, once it has closed a stream that failed (ErrorDisconnected, as a rule).
void AudioIO::onErrorAfterClose(oboe::AudioStream*, oboe::Result error)
{
    AppLog("Nano Q audio: stream closed (%s)", oboe::convertToText(error));
    mRestart = true;
}
} // namespace nanoq::android
