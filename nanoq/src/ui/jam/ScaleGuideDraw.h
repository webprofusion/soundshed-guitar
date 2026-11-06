#pragma once
// ScaleGuideDraw - the Scale Guide's neck and chord boxes (the web UI's ts/scaleGuide/neck.ts and
// chordBox.ts), drawn from the engine's view model (core/src/music/ScaleGuideView.h). Nano Q keeps
// no music theory: every dot, shape, finger and barre is the engine's.

#include "ui/Drawn.h"
#include "ui/widgets/Pressable.h"
#include "ui/widgets/Theme.h"

#include <nlohmann/json.hpp>

#include <elements.hpp>

#include <string>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;

struct GuideDot
{
    int string = 0;
    int fret = 0;
    int midi = 0;
    std::string name;
    std::string interval;
    int degree = 1;
    bool root = false;
    bool inPosition = true;
};

struct GuideBarre
{
    int fret = 0;
    int fromString = 0;
    int toString = 0;
};

struct GuideShape
{
    std::vector<int> frets; ///< -1 muted, else the fret (the capo's fret for an open string)
    std::vector<int> fingers;
    std::vector<GuideBarre> barres;
    int position = 0;
    std::string caged;
};

/// What the neck and boxes draw, read from a scaleGuideView model.
struct GuideModel
{
    std::vector<int> openMidi;
    std::vector<std::string> stringNames;
    int frets = 24;
    int capo = 0;
    bool chord = false;
    int position = 0;
    int rootPitchClass = 0;
    std::vector<GuideDot> dots;
    std::vector<GuideShape> shapes;
};

[[nodiscard]] GuideModel ReadGuideModel(const nlohmann::json& view);
[[nodiscard]] GuideShape ReadGuideShape(const nlohmann::json& shape);

/// The neck: every tone of the scale or chord, the lowest string at the bottom, the root filled,
/// the rest of the neck dimmed around a chosen position. LISTEN marks the played note and the two
/// before it (SetHeard); a played note with no dot gets a ring.
class ScaleNeck final : public el::element, public Drawn
{
  public:
    ScaleNeck(const Theme& theme, GuideModel model, std::string labels, bool leftHanded);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

    /// The note now first, then the trail; returns true when it changed.
    bool SetHeard(std::vector<int> notes);

  private:
    const Theme& mTheme;
    GuideModel mModel;
    std::string mLabels;
    bool mLeftHanded;
    std::vector<int> mHeard;
};

/// One chord shape as a chord box: a five-fret grid, × and o over the strings, finger numbers, a bar
/// for a barre, the starting fret when it is up the neck, and a caption (its CAGED name, or the
/// progression step it plays).
class ChordBoxView final : public Pressable
{
  public:
    ChordBoxView(const Theme& theme, GuideShape shape, std::vector<int> openMidi, int capo, int rootPitchClass,
                 bool leftHanded, std::string caption, std::string title, bool selected);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

  private:
    const Theme& mTheme;
    GuideShape mShape;
    std::vector<int> mOpenMidi;
    int mCapo;
    int mRootPitchClass;
    bool mLeftHanded;
    std::string mCaption;
    std::string mTitle;
    bool mSelected;
};
} // namespace nanoq::ui
