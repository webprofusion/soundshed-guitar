#pragma once

// MetronomeClickLibrary — the click sounds: which kits exist, where their
// samples are, and how to load them at the current sample rate.
//
// The shipped kits are data, not code: `metronome/kits.json` next to the WAVs
// lists them, so adding a kit is dropping a folder and a manifest line. App
// settings (`metronome.clickConfig`) are layered on top by id, which is what
// keeps a user's own click sounds working — an override with a built-in id
// replaces that kit, an unknown id appends one.
//
// Message thread only. It hands MetronomeService an immutable ClickSamples the
// audio thread reads through an atomic shared_ptr.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "controller/internal/MetronomeSupport.h"

namespace guitarfx
{
class IPluginHost;

class MetronomeClickLibrary
{
  public:
    struct ClickTypeConfig
    {
        std::string id;
        std::string label;
        std::filesystem::path lowPath;
        std::filesystem::path highPath;
        std::filesystem::path subPath;
    };

    /// One kit decoded and resampled. Channels within a slot are the same
    /// length, so Render() can index them together.
    struct ClickSamples
    {
        std::vector<std::vector<float>> low;
        std::vector<std::vector<float>> high;
        std::vector<std::vector<float>> sub;

        /// The channels a voice should play, falling back when a kit ships no
        /// sample for it: a subdivision borrows the low click, and either of
        /// low/high stands in for the other. Null when the kit is empty.
        [[nodiscard]] const std::vector<std::vector<float>>* Voice(controller_detail::ClickVoice voice) const;

        [[nodiscard]] bool Empty() const;
    };

    MetronomeClickLibrary(IPluginHost& host, const std::filesystem::path& resourceRoot);

    /// Rebuilds the catalogue from the bundled manifest plus `overrides`, the
    /// `metronome.clickConfig` array from app settings (may be null/absent).
    void Rebuild(const nlohmann::json* overrides);

    [[nodiscard]] const std::vector<ClickTypeConfig>& Types() const
    {
        return mTypes;
    }

    [[nodiscard]] bool Empty() const
    {
        return mTypes.empty();
    }

    /// The named kit, or the first one when the id is unknown — a kit that has
    /// been removed must not leave the metronome silent. Null only when the
    /// catalogue is empty.
    [[nodiscard]] const ClickTypeConfig* Find(const std::string& id) const;

    /// Decodes and resamples a kit. Null when nothing loadable was found.
    [[nodiscard]] std::shared_ptr<ClickSamples> Load(const ClickTypeConfig& config, double targetSampleRate) const;

  private:
    /// Resolves a manifest/settings path against the bundled assets and the
    /// resource root; absolute paths are taken as given.
    [[nodiscard]] std::filesystem::path ResolvePath(const std::string& rawPath) const;

    /// Reads `metronome/kits.json`. Empty when it is missing or unreadable,
    /// which is what makes the built-in fallback below matter.
    [[nodiscard]] nlohmann::json LoadManifest() const;

    /// Appends entries from a clickConfig-shaped array, replacing any existing
    /// entry with the same id. Entries whose samples are all missing are
    /// skipped so the picker never offers a silent kit.
    void AppendEntries(const nlohmann::json& entries);

    IPluginHost& mHost;
    const std::filesystem::path& mResourceRoot;
    std::vector<ClickTypeConfig> mTypes;
};
} // namespace guitarfx
