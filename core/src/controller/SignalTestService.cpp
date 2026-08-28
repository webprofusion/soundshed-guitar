#include "controller/SignalTestService.h"

#include <cmath>
#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{

namespace
{
    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

    /// Output RMS above this counts as "audio reached the output". Well below
    /// anything audible, but far enough above denormal noise that a chain
    /// muted end to end still reads as a failure.
    constexpr double kPassOutputRms = 0.001;
}

SignalTestService::SignalTestService(SendMessageFn sendMessage)
    : mSendMessage(std::move(sendMessage))
{
}

bool SignalTestService::Start(double frequencyHz, double durationSeconds, double sampleRate)
{
    if (sampleRate <= 0.0)
        return false;

    auto& st = mState;
    st.frequencyHz = frequencyHz;
    st.sampleRate = sampleRate;
    st.phase = 0.0;
    st.phaseIncrement = frequencyHz / sampleRate;
    st.totalSamples = static_cast<int>(durationSeconds * sampleRate);
    st.samplesRemaining = st.totalSamples;
    st.inputSumSquares = 0.0;
    st.outputSumSquares = {0.0, 0.0};
    st.startTime = std::chrono::steady_clock::now();

    mResult = {};
    mActive.store(true, std::memory_order_release);
    return true;
}

void SignalTestService::InjectInput(float** inputs, int numSamples)
{
    if (!mActive.load(std::memory_order_acquire))
        return;

    auto& st = mState;
    if (inputs && inputs[0] && inputs[1])
    {
        for (int i = 0; i < numSamples && st.samplesRemaining > 0; ++i, --st.samplesRemaining)
        {
            const auto sample = static_cast<float>(std::sin(st.phase * kTwoPi));
            st.phase += st.phaseIncrement;
            if (st.phase >= 1.0) st.phase -= 1.0;
            inputs[0][i] = sample;
            inputs[1][i] = sample;
            st.inputSumSquares += static_cast<double>(sample) * sample;
        }
    }

    if (st.samplesRemaining <= 0)
    {
        mActive.store(false, std::memory_order_release);
        mResultPending.store(true, std::memory_order_release);
    }
}

void SignalTestService::CollectOutput(float* const* outputs, int numSamples)
{
    if (mState.samplesRemaining <= 0 && !mResultPending.load(std::memory_order_relaxed))
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        if (outputs && outputs[0])
            mState.outputSumSquares[0] += static_cast<double>(outputs[0][i]) * outputs[0][i];
        if (outputs && outputs[1])
            mState.outputSumSquares[1] += static_cast<double>(outputs[1][i]) * outputs[1][i];
    }
}

void SignalTestService::OnIdle()
{
    if (!mResultPending.load(std::memory_order_acquire))
        return;
    mResultPending.store(false, std::memory_order_release);

    const auto& st = mState;
    const auto elapsed = std::chrono::steady_clock::now() - st.startTime;
    const int total = st.totalSamples;

    mResult.elapsedSeconds = std::chrono::duration<double>(elapsed).count();
    mResult.sampleRate = st.sampleRate;
    mResult.frequencyHz = st.frequencyHz;
    mResult.durationSeconds = static_cast<double>(st.totalSamples) / st.sampleRate;
    mResult.inputRMS = (total > 0) ? std::sqrt(st.inputSumSquares / total) : 0.0;
    mResult.outputRMS[0] = (total > 0) ? std::sqrt(st.outputSumSquares[0] / total) : 0.0;
    mResult.outputRMS[1] = (total > 0) ? std::sqrt(st.outputSumSquares[1] / total) : 0.0;
    mResult.passed = mResult.outputRMS[0] > kPassOutputRms || mResult.outputRMS[1] > kPassOutputRms;

    nlohmann::json result;
    result["type"] = "signalPathTestResult";
    result["sampleRate"] = mResult.sampleRate;
    result["frequency"] = mResult.frequencyHz;
    result["duration"] = mResult.durationSeconds;
    result["elapsed"] = mResult.elapsedSeconds;
    result["inputRMS"] = mResult.inputRMS;
    result["outputRMS"] = { mResult.outputRMS[0], mResult.outputRMS[1] };
    result["passed"] = mResult.passed;

    if (mSendMessage)
        mSendMessage(result.dump());
}

} // namespace guitarfx
