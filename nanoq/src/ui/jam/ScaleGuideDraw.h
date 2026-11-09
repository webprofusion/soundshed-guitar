#pragma once
// ScaleGuideDraw - the Scale Guide's neck, chord boxes and arpeggio tab (the web UI's
// ts/scaleGuide/neck.ts, chordBox.ts and runTab.ts), drawn from the engine's view model
// (core/src/music/ScaleGuideView.h). Nano Q keeps no music theory: every dot, shape, finger and barre is the engine's.

#include "ui/Drawn.h"
#include "ui/widgets/DragScroll.h"
#include "ui/widgets/Pressable.h"
#include "ui/widgets/Theme.h"

#include <nlohmann/json.hpp>

#include <elements.hpp>

#include <functional>
#include <optional>
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
    int order = 0;   ///< its place in the chosen arpeggio run from 1, or 0
    int finger = -1; ///< its finger in that run (0 open), or -1
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

struct GuideRunNote
{
    int string = 0;
    int fret = 0;
    std::string interval;
    bool root = false;
};

struct GuideRunStep
{
    int note = 0;
    std::string stroke; ///< "down", "up", "hammer" or "pull"
};

/// One way through a position, an arpeggio's or a scale's: its notes lowest first, its steps up them
/// and back.
struct GuideRun
{
    std::string name;
    std::string start;
    int fretFrom = 0;
    int fretTo = 0;
    std::vector<GuideRunNote> notes;
    std::vector<GuideRunStep> steps;
};

/// What the neck and boxes draw, read from a scaleGuideView model.
struct GuideModel
{
    std::vector<int> openMidi;
    std::vector<std::string> stringNames;
    int frets = 24;
    int capo = 0;
    bool chord = false;
    bool arpeggio = false;
    int position = 0;
    int rootPitchClass = 0;
    std::vector<GuideDot> dots;
    std::vector<GuideShape> shapes;
    /// Every arpeggio run, for the position buttons.
    std::vector<GuideRun> runs;
    /// The chosen arpeggio run or scale position.
    std::optional<GuideRun> chosen;

    [[nodiscard]] const GuideRun* Run() const;
};

[[nodiscard]] GuideModel ReadGuideModel(const nlohmann::json& view);
[[nodiscard]] GuideShape ReadGuideShape(const nlohmann::json& shape);

/// The neck: every tone of the scale or chord, the lowest string at the bottom, the root filled,
/// the rest of the neck dimmed around a chosen position. LISTEN marks the played note and the two
/// before it (SetHeard); a played note with no dot gets a ring.
class ScaleNeck final : public el::element, public Drawn, public TapTarget
{
  public:
    ScaleNeck(const Theme& theme, GuideModel model, std::string labels, bool leftHanded);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;

    bool wants_control() const override
    {
        return true;
    }

    /// The note now first, then the trail; returns true when it changed.
    bool SetHeard(std::vector<int> notes);

    /// A note tapped on the neck.
    std::function<void(int midi)> onNote;

  private:
    const Theme& mTheme;
    GuideModel mModel;
    std::string mLabels;
    bool mLeftHanded;
    std::vector<int> mHeard;
    int mPressedNote = -1;

    /// The note of the dot under `point`, or -1.
    [[nodiscard]] int NoteAt(const el::context& ctx, el::point point) const;
};

/// A run as tab, read left to right for either hand: the frets in playing order with the
/// pick strokes above and each note's interval below. The highest string is on top.
class RunTabView final : public el::element, public Drawn
{
  public:
    RunTabView(const Theme& theme, GuideRun run, std::vector<std::string> stringNames);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;

  private:
    const Theme& mTheme;
    GuideRun mRun;
    std::vector<std::string> mStringNames;
};

/// What lies under it plays when clicked: a face that shows hover, and stays lit while it plays.
class PlayArea final : public Pressable
{
  public:
    PlayArea(const Theme& theme, bool playing) : mTheme(theme), mPlaying(playing)
    {
    }

    void draw(const el::context& ctx) override;

  private:
    const Theme& mTheme;
    bool mPlaying;
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
