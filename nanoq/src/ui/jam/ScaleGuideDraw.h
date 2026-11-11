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
#include <utility>
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
    std::string stroke; ///< "down", "up", "hammer", "pull" or "slide"
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

/// One chord of a progression's arpeggios: which of the piece's steps are its bar.
struct GuideBar
{
    int chord = 0;
    std::string name;
    std::string numeral;
    int from = 0;
    int count = 0;
};

/// A progression's arpeggios end to end (the view's `progressionRun`): one run, a bar for each chord.
struct GuideProgressionRun
{
    GuideRun run;
    bool slides = false;
    std::vector<GuideBar> bars;
};

/// The view's progression run, when it has one.
[[nodiscard]] std::optional<GuideProgressionRun> ReadGuideProgressionRun(const nlohmann::json& view);

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

    /// The note now first, then the trail; returns true when it changed. `everywhere` marks every
    /// place a note is on the neck; without it only the places inside the chosen position are
    /// marked, unless the note is nowhere in it.
    bool SetHeard(std::vector<int> notes, bool everywhere = true);

    /// The places (string, fret) Play is sounding now, lit; returns true when they changed.
    bool SetPlaying(std::vector<std::pair<int, int>> places);

    /// A note tapped on the neck.
    std::function<void(int midi)> onNote;

  private:
    const Theme& mTheme;
    GuideModel mModel;
    std::string mLabels;
    bool mLeftHanded;
    std::vector<int> mHeard;
    bool mHeardEverywhere = true;
    std::vector<std::pair<int, int>> mPlaying;
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

    /// The steps of the run Play is sounding now, lit; returns true when they changed.
    bool SetPlaying(std::vector<int> steps);

  private:
    const Theme& mTheme;
    GuideRun mRun;
    std::vector<std::string> mStringNames;
    std::vector<int> mPlaying;
};

/// A progression's arpeggios as one diagram (the web UI's runTab.ts with a flow): it runs on to new
/// lines at the width given, a bar line after each chord, the chord's name over its first note and
/// the string names at the start of each line. A tap on a chord's bar reports that chord.
class ProgressionTabView final : public el::element, public Drawn, public TapTarget
{
  public:
    ProgressionTabView(const Theme& theme, GuideProgressionRun whole, std::vector<std::string> stringNames, float width,
                       int activeChord);

    el::view_limits limits(const el::basic_context& ctx) const override;
    void draw(const el::context& ctx) override;
    bool click(const el::context& ctx, el::mouse_button btn) override;

    bool wants_control() const override
    {
        return true;
    }

    /// The steps of the piece Play is sounding now, lit; returns true when they changed.
    bool SetPlaying(std::vector<int> steps);

    /// A chord's bar tapped: its index in the progression.
    std::function<void(int chord)> onBar;

  private:
    /// One chord's stretch of one line: a bar that does not fit a line carries on as another.
    struct Segment
    {
        std::size_t bar = 0;
        int line = 0;
        float from = 0.0f;
        float to = 0.0f;
        bool first = true;
        bool last = true;
        /// Each step's index in the run and where its number sits.
        std::vector<std::pair<int, float>> cells;
    };

    const Theme& mTheme;
    GuideProgressionRun mWhole;
    std::vector<std::string> mStringNames;
    int mActive;
    std::vector<Segment> mSegments;
    int mLines = 1;
    float mWidth = 0.0f;
    std::vector<int> mPlaying;
    int mPressedBar = -1;

    [[nodiscard]] float LineHeight() const;
    /// The chord whose bar is under `point`, or -1.
    [[nodiscard]] int BarAt(const el::context& ctx, el::point point) const;
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
