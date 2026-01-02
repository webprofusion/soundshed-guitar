/**
 * @file OfflineProcessingTest.cpp
 * @brief Offline audio file processing test application
 *
 * This application loads a preset with model and IR, processes an input WAV file
 * through the complete DSP pipeline, and writes the output to a WAV file for
 * manual review and comparison with live processing.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/NAMDSPManager.h"
#include "presets/PresetManager.h"
#include "IPlugConstants.h"

// Force factory registration
#include "NAM/wavenet.h"
#include "NAM/lstm.h"
#include "NAM/convnet.h"

namespace
{
[[maybe_unused]] volatile auto force_wavenet = &nam::wavenet::Factory;
[[maybe_unused]] volatile auto force_lstm = &nam::lstm::Factory;
[[maybe_unused]] volatile auto force_convnet = &nam::convnet::Factory;

namespace fs = std::filesystem;

/**
 * @brief Simple WAV file reader/writer
 */
class WavFile
{
public:
  struct Header
  {
    // RIFF chunk
    char riffId[4];           // "RIFF"
    uint32_t fileSize;        // File size - 8
    char waveId[4];           // "WAVE"
    
    // fmt chunk
    char fmtId[4];            // "fmt "
    uint32_t fmtSize;         // Usually 16
    uint16_t audioFormat;     // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    
    // data chunk
    char dataId[4];           // "data"
    uint32_t dataSize;        // Number of bytes in data
  };

  static bool Read(const std::string& filename, std::vector<std::vector<double>>& channels,
                   double& sampleRate)
  {
    std::ifstream file(filename, std::ios::binary);
    if (!file)
    {
      std::cerr << "Failed to open input file: " << filename << "\n";
      return false;
    }

    Header header;
    file.read(reinterpret_cast<char*>(&header), sizeof(Header));

    // Validate header
    if (std::memcmp(header.riffId, "RIFF", 4) != 0 ||
        std::memcmp(header.waveId, "WAVE", 4) != 0 ||
        std::memcmp(header.fmtId, "fmt ", 4) != 0 ||
        std::memcmp(header.dataId, "data", 4) != 0)
    {
      std::cerr << "Invalid WAV file format\n";
      return false;
    }

    if (header.audioFormat != 1)
    {
      std::cerr << "Only PCM format supported\n";
      return false;
    }

    sampleRate = header.sampleRate;
    const int numChannels = header.numChannels;
    const int bitsPerSample = header.bitsPerSample;
    const int numSamples = header.dataSize / (numChannels * (bitsPerSample / 8));

    std::cout << "Input WAV: " << numChannels << " channels, " 
              << sampleRate << " Hz, " << bitsPerSample << " bits, "
              << numSamples << " samples\n";

    // Read audio data
    channels.resize(numChannels);
    for (auto& channel : channels)
    {
      channel.resize(numSamples);
    }

    if (bitsPerSample == 16)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        for (int ch = 0; ch < numChannels; ++ch)
        {
          int16_t sample;
          file.read(reinterpret_cast<char*>(&sample), sizeof(int16_t));
          channels[ch][i] = sample / 32768.0;
        }
      }
    }
    else if (bitsPerSample == 24)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        for (int ch = 0; ch < numChannels; ++ch)
        {
          uint8_t bytes[3];
          file.read(reinterpret_cast<char*>(bytes), 3);
          int32_t sample = (bytes[2] << 24) | (bytes[1] << 16) | (bytes[0] << 8);
          sample >>= 8; // Sign extend
          channels[ch][i] = sample / 8388608.0;
        }
      }
    }
    else if (bitsPerSample == 32)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        for (int ch = 0; ch < numChannels; ++ch)
        {
          int32_t sample;
          file.read(reinterpret_cast<char*>(&sample), sizeof(int32_t));
          channels[ch][i] = sample / 2147483648.0;
        }
      }
    }
    else
    {
      std::cerr << "Unsupported bit depth: " << bitsPerSample << "\n";
      return false;
    }

    return true;
  }

  static bool Write(const std::string& filename, const std::vector<std::vector<double>>& channels,
                    double sampleRate, int bitsPerSample = 16)
  {
    if (channels.empty() || channels[0].empty())
    {
      std::cerr << "No audio data to write\n";
      return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file)
    {
      std::cerr << "Failed to open output file: " << filename << "\n";
      return false;
    }

    const int numChannels = static_cast<int>(channels.size());
    const int numSamples = static_cast<int>(channels[0].size());
    const int bytesPerSample = bitsPerSample / 8;

    Header header;
    std::memcpy(header.riffId, "RIFF", 4);
    std::memcpy(header.waveId, "WAVE", 4);
    std::memcpy(header.fmtId, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;
    header.numChannels = numChannels;
    header.sampleRate = static_cast<uint32_t>(sampleRate);
    header.bitsPerSample = bitsPerSample;
    header.blockAlign = numChannels * bytesPerSample;
    header.byteRate = header.sampleRate * header.blockAlign;
    std::memcpy(header.dataId, "data", 4);
    header.dataSize = numSamples * numChannels * bytesPerSample;
    header.fileSize = 36 + header.dataSize;

    file.write(reinterpret_cast<const char*>(&header), sizeof(Header));

    // Write audio data
    if (bitsPerSample == 16)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        for (int ch = 0; ch < numChannels; ++ch)
        {
          double sample = std::clamp(channels[ch][i], -1.0, 1.0);
          int16_t intSample = static_cast<int16_t>(sample * 32767.0);
          file.write(reinterpret_cast<const char*>(&intSample), sizeof(int16_t));
        }
      }
    }
    else if (bitsPerSample == 24)
    {
      for (int i = 0; i < numSamples; ++i)
      {
        for (int ch = 0; ch < numChannels; ++ch)
        {
          double sample = std::clamp(channels[ch][i], -1.0, 1.0);
          int32_t intSample = static_cast<int32_t>(sample * 8388607.0);
          uint8_t bytes[3];
          bytes[0] = (intSample) & 0xFF;
          bytes[1] = (intSample >> 8) & 0xFF;
          bytes[2] = (intSample >> 16) & 0xFF;
          file.write(reinterpret_cast<const char*>(bytes), 3);
        }
      }
    }

    std::cout << "Output WAV written: " << numChannels << " channels, " 
              << sampleRate << " Hz, " << bitsPerSample << " bits, "
              << numSamples << " samples\n";

    return true;
  }
};

nlohmann::json LoadJson(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
  {
    throw std::runtime_error("Failed to open JSON file: " + path.string());
  }

  nlohmann::json document;
  input >> document;
  return document;
}

} // anonymous namespace

int main(int argc, char* argv[])
{
  std::cout << "NAMGuitarFX Offline Processing Test\n";
  std::cout << "====================================\n\n";

  try
  {
    // Parse command line arguments
    std::string inputFile = "guitar-riff-02.wav";
    std::string outputFile = "guitar-riff-02-processed.wav";
    std::string modelPath = "";
    std::string irPath = "";
    std::string resourcesDir = NAMGUITAR_TEST_RESOURCES_DIR;

    if (argc >= 2)
      inputFile = argv[1];
    if (argc >= 3)
      outputFile = argv[2];
    if (argc >= 4)
      modelPath = argv[3];
    if (argc >= 5)
      irPath = argv[4];

    std::cout << "Input file:     " << inputFile << "\n";
    std::cout << "Output file:    " << outputFile << "\n";
    std::cout << "Resources dir:  " << resourcesDir << "\n";
    if (!modelPath.empty())
      std::cout << "Model:          " << modelPath << "\n";
    if (!irPath.empty())
      std::cout << "IR:             " << irPath << "\n";
    std::cout << "\n";

    // Load input WAV file
    std::vector<std::vector<double>> inputChannels;
    double sampleRate = 0;

    if (!WavFile::Read(inputFile, inputChannels, sampleRate))
    {
      return 1;
    }

    const int numSamples = static_cast<int>(inputChannels[0].size());
    const int numInputChannels = static_cast<int>(inputChannels.size());

    // Ensure we have at least 2 channels (mono input becomes dual mono)
    if (numInputChannels == 1)
    {
      inputChannels.push_back(inputChannels[0]);
    }

    // Create DSP manager
    auto dspManager = std::make_unique<namguitar::NAMDSPManager>();
    constexpr int kBlockSize = 512;
    dspManager->Prepare(sampleRate, kBlockSize);

    // Set default DSP parameters
    dspManager->SetInputTrim(0.0);
    dspManager->SetOutputTrim(0.0);
    dspManager->SetDrive(0.1);
    dspManager->SetTone(0.5);
    dspManager->SetGateEnabled(false);

    // Load model if specified
    if (!modelPath.empty())
    {
      // Make path absolute if relative
      fs::path fullModelPath = modelPath;
      if (fullModelPath.is_relative())
      {
        fullModelPath = fs::path(resourcesDir) / modelPath;
      }

      std::cout << "Loading NAM model from: " << fullModelPath << "\n";
      if (dspManager->LoadModel(fullModelPath.string()))
      {
        std::cout << "NAM model loaded successfully\n";
        dspManager->SetAmpEnabled(true);
      }
      else
      {
        std::cerr << "Failed to load NAM model\n";
        return 1;
      }
    }
    else
    {
      std::cout << "No model specified, amp disabled\n";
      dspManager->SetAmpEnabled(false);
    }

    // Load IR if specified
    if (!irPath.empty())
    {
      // Make path absolute if relative
      fs::path fullIrPath = irPath;
      if (fullIrPath.is_relative())
      {
        fullIrPath = fs::path(resourcesDir) / irPath;
      }

      std::cout << "Loading IR from: " << fullIrPath << "\n";
      if (dspManager->LoadImpulseResponse(fullIrPath.string()))
      {
        std::cout << "IR loaded successfully\n";
        dspManager->SetCabEnabled(true);
      }
      else
      {
        std::cerr << "Failed to load IR\n";
        return 1;
      }
    }
    else
    {
      std::cout << "No IR specified, cab disabled\n";
      dspManager->SetCabEnabled(false);
    }

    std::cout << "\nProcessing audio...\n";

    // Prepare output buffers
    std::vector<std::vector<double>> outputChannels(2);
    outputChannels[0].resize(numSamples);
    outputChannels[1].resize(numSamples);

    // Process audio in blocks
    int processedSamples = 0;
    int blockCount = 0;

    while (processedSamples < numSamples)
    {
      int remainingSamples = numSamples - processedSamples;
      int currentBlockSize = std::min(kBlockSize, remainingSamples);

      // Prepare iPlug buffers
      iplug::sample inputBuffer[2][512];
      iplug::sample outputBuffer[2][512];

      for (int i = 0; i < currentBlockSize; ++i)
      {
        inputBuffer[0][i] = inputChannels[0][processedSamples + i];
        inputBuffer[1][i] = inputChannels[1][processedSamples + i];
      }

      // Process through DSP
      iplug::sample* inputs[] = { inputBuffer[0], inputBuffer[1] };
      iplug::sample* outputs[] = { outputBuffer[0], outputBuffer[1] };
      dspManager->Process(inputs, outputs, currentBlockSize);

      // Copy to output
      for (int i = 0; i < currentBlockSize; ++i)
      {
        outputChannels[0][processedSamples + i] = outputBuffer[0][i];
        outputChannels[1][processedSamples + i] = outputBuffer[1][i];
      }

      processedSamples += currentBlockSize;
      blockCount++;

      // Progress indicator
      if (blockCount % 100 == 0)
      {
        double progress = 100.0 * processedSamples / numSamples;
        std::cout << "\rProcessing: " << static_cast<int>(progress) << "%..." << std::flush;
      }
    }

    std::cout << "\rProcessing: 100% (" << blockCount << " blocks)\n\n";

    // Write output WAV file
    if (!WavFile::Write(outputFile, outputChannels, sampleRate, 16))
    {
      return 1;
    }

    std::cout << "\nProcessing complete!\n";
    std::cout << "Output written to: " << outputFile << "\n";

    return 0;
  }
  catch (const std::exception& e)
  {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
