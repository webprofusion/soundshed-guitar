#pragma once

#include "IRTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace namguitar
{
  class IRManager
  {
  public:
    IRManager() = default;

    bool LoadImpulseResponse(const std::filesystem::path &filePath, double targetSampleRate);
    void Clear();
    [[nodiscard]] std::optional<std::filesystem::path> CurrentImpulseResponse() const;
    [[nodiscard]] const std::vector<float> &Impulse() const noexcept;
    [[nodiscard]] bool HasImpulse() const noexcept;
    
    // IR quality settings
    void SetQuality(IRQuality quality) { mQuality = quality; }
    [[nodiscard]] IRQuality GetQuality() const noexcept { return mQuality; }
    
    // Get the processed (possibly truncated) impulse for convolution
    [[nodiscard]] std::vector<float> GetProcessedImpulse(double sampleRate) const;
    
    // Get info about the current IR
    [[nodiscard]] size_t GetOriginalLength() const noexcept { return mImpulse.size(); }
    [[nodiscard]] size_t GetProcessedLength(double sampleRate) const;

    // Test-only method to set impulse data directly without loading from file
    void SetImpulse(const std::vector<float>& impulse);
    
    // Static utility: find optimal truncation point based on energy threshold
    // Returns the sample index where cumulative energy reaches (1 - threshold) of total
    // e.g., threshold = 0.001 means find where 99.9% of energy is captured
    [[nodiscard]] static size_t FindEnergyTruncationPoint(
      const std::vector<float>& samples, 
      float threshold = 0.001f);

  private:
    bool ParseWavFile(std::ifstream &stream, double targetSampleRate);
    bool ParseRiffHeader(std::ifstream &stream);
    bool ParseFmtChunk(std::ifstream &stream, std::uint16_t &audioFormat, std::uint16_t &channels,
                       std::uint32_t &sampleRate, std::uint16_t &bitsPerSample, std::uint16_t &blockAlign,
                       std::uint32_t chunkSize);
    bool ParseDataChunk(std::ifstream &stream, std::uint16_t audioFormat, std::uint16_t bitsPerSample, std::uint16_t channels,
                        std::uint32_t dataSize, double targetSampleRate, std::uint32_t sourceSampleRate);

    std::optional<std::filesystem::path> mCurrentIR;
    std::vector<float> mImpulse;
    double mImpulseSampleRate = 0.0;
    IRQuality mQuality = IRQuality::Standard;
  };
} // namespace namguitar
