#pragma once

// Shell - the frame every page sits in. At or below the web UI's compact breakpoint (980 x 620,
// core/ui/ts/compactMode.ts; the ui.density setting can force either) it is laid out as the web
// UI's compact landscape layout (html[data-compact-layout="rail"]): a rail at the left that switches between the Chain
// and Effect stages (the two halves of the Play tab, which take turns at this size) and opens Settings; the preset bar
// along the top; the stage; and the footer with its Options sheet. Presets and Settings take the stage's place when
// open.
//
// Above it, the full layout: the PLAY / TONES / PRESETS tabs along the top, the control bar
// (input, gate, transpose, the preset toolbar and selector, EQ, doubler, output), the chain with
// the selected effect's editor under it, and the full footer. The same regions are rebuilt; the
// frame around them is swapped when the window crosses the breakpoint.
//
// The UI is rebuilt from the client's state, region by region, rather than patched: what a
// region shows is a pure function of ClientState, so a change just marks the regions it
// touches and the next tick (on the UI thread, between events) rebuilds them. What moves on its
// own - knob and slider values, meters, lights, readouts - is bound as each region builds
// (mLive, Bindings.h) and patched in place each tick, since a rebuild would be visible and would
// drop a drag.

#include "WebView.h"
#include "ui/input/AccessBridge.h"
#include "ui/widgets/Bindings.h"
#include "ui/widgets/Graphics.h"
#include "ui/input/KeyRouter.h"
#include "ui/widgets/Layout.h"
#include "ui/effects/LayoutView.h"
#include "ui/effects/ResponsePlot.h"
#include "ui/chain/EffectRows.h"
#include "ui/shell/FrameWidgets.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Panels.h"
#include "ui/UiSession.h"
#include "ui/widgets/Widgets.h"
#include "uiclient/ChainDropRules.h"
#include "uiclient/ChainHistory.h"
#include "uiclient/ChainLayout.h"
#include "uiclient/DspStatus.h"
#include "uiclient/SignalFlow.h"
#include "uiclient/UpdateCheck.h"
#include "uiclient/EffectPresentation.h"
#include "uiclient/Instruments.h"
#include "uiclient/PracticeTool.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/PresetTrail.h"
#include "uiclient/ScaleGuide.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nanoq::ui
{
enum class Page
{
    Chain,
    Effect,
    Tones,
    Jam,
    Pads, // the performance pads: under the chain in the full layout, a page of their own in the compact
    Settings
};

/// One row of a pop-up list.
struct MenuItem
{
    std::string label;
    bool selected = false;
    bool heading = false;
    std::function<void()> onChoose;
    std::string detail; // muted, at the right
    std::string icon;   // an icon name (images/icons), drawn at the left in `tint`
    std::uint32_t tint = 0;
    std::string subtitle; // a second, muted line under the label (a drop-down's description)
};

/// What a dialog holds, for Shell::ShowDialog: the web UI's modal, with its title and a close
/// cross in the header band, the body, and the action buttons in a footer band.
struct DialogParts
{
    std::string title;
    Ptr body;
    /// The footer's buttons, left to right at the right edge. None: no footer band.
    std::vector<Ptr> actions;
    /// At the footer's left, before the buttons (a status, a secondary action).
    Ptr footerLeft;
    /// In the header, between the title and the close cross (a source label, an Enabled switch).
    std::vector<Ptr> headerExtras;
    float width = 460.0f;
    float height = 300.0f;
    /// The body's padding inside the card; 0 for a body that lays out its own edges.
    float padX = 20.0f;
    float padY = 18.0f;
    /// What the close cross does, when it is more than closing (Cancel undoing a preview).
    std::function<void()> onClose;
    /// The body takes the dialog's whole height (a list that scrolls in itself), rather than
    /// sitting at its top.
    bool stretch = false;
};

/// The Tones and Jam pages' sections (Shell::WithSectionNav): a side list in the full layout, chips
/// across the top at the compact size.
struct SectionNav
{
    struct Item
    {
        std::string label;
        bool active = false;
        std::function<void()> onClick;
    };

    std::string account; // the account pill's label; empty for none
    std::function<void(el::rect anchor)> onAccount;
    std::vector<Item> items;
    std::vector<Ptr> tools;        // under the sections, in the side list
    std::vector<Ptr> compactTools; // at the right of the chips, at the compact size
    std::string help;              // a help card under the side list
};

class WebViewSlot; // JamPage.cpp
class ScaleNeck;   // ScaleGuideDraw.h
class NodeCard;    // NodeCard.h
class Connector;   // ChainWires.h

namespace chain
{
struct HitMap; // NodeCard.h
}

class Shell
{
  public:
    explicit Shell(UiSession& session);
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    [[nodiscard]] Ptr Root() const
    {
        return mRoot;
    }

    /// Rebuilds the regions the last messages touched and moves the meters. UI thread.
    void OnTick();

  private:
    enum Dirty : unsigned
    {
        kTopBar = 1u << 0,
        kScenes = 1u << 1,
        kChain = 1u << 2,
        kPage = 1u << 3,
        kTransport = 1u << 4,
        kValues = 1u << 5, // the effect page's sections drawn from its values (curve, IR alignment)
        kRail = 1u << 6,   // the compact rail, or the full layout's tab row
        kRebuilds = kTopBar | kScenes | kChain | kPage | kTransport | kRail,
        kAll = kRebuilds
    };

    void Mark(unsigned bits)
    {
        mDirty |= bits;
    }

    void Subscribe();
    void BuildRoot();
    void Rebuild();

    /// The keyboard shortcuts (Keys.cpp), as the web UI's: undo and redo, Space, Delete, Esc, F2.
    bool HandleKey(const el::key_info& k);
    /// Space with no effect picked out: a tap on the tempo.
    void TapTempo();
    /// A layer of the view's own (a dialog, a drop-down), which takes keys while it has focus,
    /// wrapped so the shortcuts (Esc above all) still reach HandleKey.
    Ptr KeyLayer(Ptr layer);
    /// A layer at the foot of the view that gives way to the on-screen keyboard.
    Ptr AvoidKeyboard(Ptr layer);
    void TickKeyboard();
    /// The standalone window's caption takes the colour of the bar under it (Windows).
    void SyncWindowChrome();

    // The window's size and which layout it gets (ShellFrame.cpp).
    /// The view's size in layout units: the zoom divided out.
    [[nodiscard]] el::extent ViewSize() const;
    [[nodiscard]] bool WantsWide() const;

    /// Where the stage starts and how wide it is: right of the rail when compact.
    [[nodiscard]] float StageLeft() const
    {
        return mWide ? 0.0f : metrics::kRailWidth;
    }

    [[nodiscard]] float StageWidth() const
    {
        return ViewSize().x - StageLeft();
    }

    /// The frame for the layout in force, around the region slots.
    Ptr BuildFrame();

    /// Chain and Effect are one page in the full layout.
    [[nodiscard]] bool PlayShown() const
    {
        return mPage == Page::Effect || (mWide && mPage == Page::Chain);
    }

    /// The full layout's play stage is up: the chain over the editor, or over the pads.
    [[nodiscard]] bool PlayStageShown() const
    {
        return mWide && (PlayShown() || mPage == Page::Pads);
    }

    /// The footer's pads button: the pads, or back from them.
    void TogglePads()
    {
        ShowPage(mPage == Page::Pads ? (mWide ? Page::Effect : Page::Chain) : Page::Pads);
    }

    // Shell.cpp
    Ptr BuildRail();
    Ptr BuildTopBar();
    Ptr BuildScenes();
    Ptr BuildTransport();
    Ptr BuildStarting();
    Ptr BuildChainStage();
    /// The footer's theme drop-down, in either frame.
    [[nodiscard]] std::shared_ptr<IconButton> BuildThemeButton();

    // The full layout (WideFrame.cpp).
    Ptr BuildTabBar();
    Ptr BuildHeader();
    Ptr BuildPlayStage();
    Ptr BuildWideFooter();
    /// The demo clip chooser and its play button, as both footers show them.
    [[nodiscard]] Ptr BuildDemoControls(float chooserWidth, bool withRepeat);
    /// The tempo, tap and the metronome's switch, as both footers show them.
    [[nodiscard]] Ptr BuildTempoControls(bool withTap);
    /// Undo, redo, the A/B pair and the copy between them, as both footers show them.
    [[nodiscard]] Ptr BuildHistoryControls();
    void PlayOrStopDemo();
    /// The zoom's select (100%, ...), for the Options sheet and the full footer.
    [[nodiscard]] Ptr ZoomButton();
    /// The presets loaded, in order, for the control bar's back and forward (preset-undo/redo).
    void NotePresetTrail();
    void StepPresetTrail(int direction);

    /// The global chain's cells (Controls.cpp): the compact Controls sheet and the full
    /// layout's control bar lay out the same ones. Their values and meters are bound (mLive).
    struct ControlCells
    {
        Ptr in, gate, transpose, eq, doubler, out;
        std::shared_ptr<MeterBar> inMeter, outMeter;
        Ptr mute;
        Ptr levelling; // input levelling, under the input meter
    };

    ControlCells BuildControlCells();
    /// A small panel beside `anchor` (a control's bounds), closed by a tap anywhere else.
    void ShowPopover(Ptr panel, el::rect anchor, float width, float height, bool above = false);
    /// The panel placed beside `anchor`, over a layer that calls `onOutside` when tapped.
    [[nodiscard]] Ptr PlacePanel(Ptr panel, el::rect anchor, float width, float height, bool above,
                                 std::function<void()> onOutside);
    void CloseDropdown();
    /// A drop-down list under `anchor`, as the web UI's menus open from their buttons.
    void OpenDropdown(el::rect anchor, std::vector<MenuItem> items, float width = 180.0f);
    /// A drop-down whose options sit in columns side by side, each `width` wide (the web UI's
    /// multi-column metro-select, for the meters).
    void OpenDropdownColumns(el::rect anchor, std::vector<std::vector<MenuItem>> columns, float width);
    void OpenOptions(el::rect anchor);
    void OpenNewPresetMenu(el::rect anchor);
    /// "New preset" for a menu: it asks first if the preset has unsaved changes.
    [[nodiscard]] MenuItem NewPresetItem();
    void LoadRandomPreset();

    // The chain's undo/redo and A/B (ShellFrame.cpp), as the web UI's footer (history.ts).
    /// An edit happened, or may have: the chain is compared and recorded once it stays still.
    void NoteChainEdit();
    void CommitChainHistory();
    void ApplyChainSnapshot(const guitarfx::uiclient::ChainSnapshot& target);
    void UndoChain();
    void RedoChain();
    void SetChainSlot(guitarfx::uiclient::ChainHistory::Slot slot);
    void CopyChainSlot();
    void ShowPage(Page page);
    void SelectNode(const std::string& nodeId);
    void EnsureSelection();
    void SelectAddedNode();
    [[nodiscard]] const guitarfx::GraphNode* SelectedNode() const;

    // ChainStrip.cpp
    Ptr BuildChain();
    /// Whether the full layout's chain is drawn as icon tiles: the bar under it dragged up
    /// past the cards' height, as the web UI's splitter does (signalPath/layout.ts).
    [[nodiscard]] bool IconChain() const;
    [[nodiscard]] std::string ChainSignature() const;
    /// Files dragged in from the desktop (ChainStrip.cpp): a .nam model or a .wav IR held over
    /// an effect that plays one lights its card; dropped, it joins the library where it is and
    /// loads into that effect, or into the one shown, as the web UI's file drop does.
    void TrackFileDrop(const el::drop_info& info, bool leaving);
    bool DropFiles(const el::drop_info& info);
    /// The effect a model or IR file dropped at `where` goes into, or empty.
    [[nodiscard]] std::string FileDropTarget(const std::string& resourceType, el::point where) const;
    /// A card being dragged: the drop target under `to` is lit, and on `done` the card moves
    /// there by the web UI's rules (ChainDropRules).
    void DragCard(const std::string& nodeId, el::point from, el::point to, bool done);
    /// What lies under `to` in the chain for a drop: a card (not `except`'s), else a cable.
    [[nodiscard]] guitarfx::uiclient::NodeDropTarget DropTargetAt(el::point to, const std::string& except);
    /// An effect dragged out of the FX Library: lights where it would land, and adds it there when
    /// let go.
    void DragLibraryEffect(const std::string& type, const std::string& name, el::point to, bool done);
    /// The round + over the chain: Add FX... (the FX Library) or Add Scene.
    void OpenAddMenu(el::rect anchor);

    [[nodiscard]] std::string SceneSignature() const;
    /// What the preset bar (or the full control bar) shows that a rebuild would change.
    [[nodiscard]] std::string TopBarSignature() const;

    // EffectPage.cpp
    [[nodiscard]] std::string EffectPageSignature() const;
    Ptr BuildEffectPage();
    void SyncEffectValues();
    void OpenEffectPresets();
    void OpenEnumPicker(const std::string& nodeId, const guitarfx::uiclient::EffectParamInfo& info);
    [[nodiscard]] Ptr BuildResourceSlot(const guitarfx::GraphNode& node, const std::string& resourceType,
                                        int resourceIndex, const std::string& label, bool optional,
                                        bool stacked = false);
    [[nodiscard]] Ptr BuildSplitBranches(const guitarfx::GraphNode& splitter);
    /// A third-party plugin node's list, Open Plugin and Browse (NodeControls.cpp).
    [[nodiscard]] Ptr BuildPluginHost(const guitarfx::GraphNode& node);
    /// A blend node's blend and its mode (NodeControls.cpp).
    [[nodiscard]] Ptr BuildBlendInfo(const guitarfx::GraphNode& node);
    /// The IR Cabinet's IR list between its slots (NodeControls.cpp).
    [[nodiscard]] Ptr BuildIrList(const guitarfx::GraphNode& node);

    // EffectLayoutPage.cpp: an effect drawn with its custom layout (the web UI's layoutRenderer.ts).
    /// The layout as one element `width` wide at most, its drawn height in `height`. A backdrop
    /// layout carries `defaultControls`, the standard controls laid out `controlsSize` big.
    [[nodiscard]] Ptr BuildEffectLayout(const guitarfx::GraphNode& node, const guitarfx::uiclient::EffectLayout& layout,
                                        float width, float availableHeight, Ptr defaultControls,
                                        el::extent controlsSize, float& height);
    /// The header's chooser between the standard controls and the layouts the effect can wear,
    /// or null when it has none to choose; `width` is what it takes.
    [[nodiscard]] Ptr BuildLayoutChooser(const guitarfx::GraphNode& node, bool usingLayout, float& width);

    // EqPlot.cpp: the EQ curves, in the effect editor and the Global EQ dialog.
    struct EqPlotSource
    {
        std::string effectType;
        guitarfx::uiclient::NodeAddress address; // where the spectrum watch points
        std::vector<std::string> feedKeys;       // where its spectrum arrives (ClientState::analysis)
        std::function<std::map<std::string, double>()> read;
        std::function<void(const std::string& key, double value, bool commit)> write;
    };

    struct EqPlotState
    {
        std::shared_ptr<ResponsePlot> plot;
        EqPlotSource source;
        std::string lastQuery; // the parameters last asked about
    };

    /// What an EQ dialog shows (EqDialog.cpp): the Global EQ or the backing-track EQ.
    struct EqDialogSpec
    {
        std::string title;
        EqPlotSource source;
        std::function<bool()> enabled;
        std::function<void(bool)> setEnabled;
        std::function<void()> reset;                    // the header's Reset, when the dialog has one
        std::function<void()> onClosed;                 // after the dialog has gone
        std::optional<guitarfx::uiclient::Topic> topic; // what moves the knobs from outside
    };

    void OpenEqDialog(EqDialogSpec spec);

    [[nodiscard]] static bool IsEqPlotType(const std::string& type);
    [[nodiscard]] Ptr MakeEqPlot(EqPlotSource source, bool overlay);
    void SyncEqPlot(EqPlotState& state);
    void SyncEqPlotFeeds();
    [[nodiscard]] EqPlotSource NodeEqPlotSource(const guitarfx::GraphNode& node);

    // GateLearn.cpp: the noise gate's Learn, in a gate node's editor and the global gate's settings,
    // and the Multi-band Gate's, one meter per band.
    struct GateLearnTarget
    {
        guitarfx::uiclient::NodeAddress address;
        std::function<double()> threshold, hysteresis;
        double minDb = -80.0, maxDb = 0.0; // the threshold's range: the meter's ends
        std::function<void(double thresholdDb)> apply;
        std::function<void()> ensureEnabled; // a bypassed gate hears nothing
        // A Multi-band Gate: its bands' labels, low band first, their thresholds as the gate
        // applies them, and what sets a finished listen's thresholds (each band has one).
        std::vector<std::string> bandLabels;
        std::function<std::vector<double>()> bandThresholds;
        std::function<void(const std::vector<guitarfx::uiclient::GateLearnBandStatus>&)> applyBands;
    };

    [[nodiscard]] Ptr BuildGateLearn(GateLearnTarget target);
    [[nodiscard]] static float GateLearnHeight(const GateLearnTarget& target);

    // FlyoutParams.cpp: a settings flyout's parameter rows (flyoutParamControls.ts).
    struct FlyoutBinding
    {
        std::function<double(const guitarfx::uiclient::EffectParamInfo&)> value;
        std::function<void(const std::string& key, double value, bool commit)> apply;
    };

    /// One row per parameter: label | knob, switch or picker | value; the rows follow the values.
    [[nodiscard]] Ptr BuildFlyoutParamRows(const std::vector<guitarfx::uiclient::EffectParamInfo>& params,
                                           const FlyoutBinding& binding);
    [[nodiscard]] GateLearnTarget NodeGateLearnTarget(const guitarfx::GraphNode& node);
    [[nodiscard]] GateLearnTarget MultibandGateLearnTarget(const guitarfx::GraphNode& node);
    void OpenGlobalGate(el::rect anchor);
    std::map<std::string, std::string> mGateLearnSeen; // each gate's Learn state when last looked at
    // InputLevelling.cpp: input levelling (the web UI's settings/inputCalibration.ts): the button
    // under the input meter, its flyout of profiles, and the training dialog.
    [[nodiscard]] Ptr BuildInputLevellingButton();
    void OpenInputLevelling(el::rect anchor);
    void OpenInputLevellingTraining();
    // DevicePage.cpp: the standalone app's audio and MIDI devices, at the top of Settings (the web
    // UI's settings/audioDevice.ts). SyncDevicePage runs every tick: the level lease, and a redraw
    // when a new device snapshot arrives. The meter is bound (mLive).
    struct DevicePage;
    [[nodiscard]] Ptr BuildDeviceSection();
    [[nodiscard]] Ptr BuildDeviceContent();
    void SyncDevicePage();
    void DeviceRequest(const std::string& action, nlohmann::json args = nlohmann::json::object());
    std::shared_ptr<DevicePage> mDevicePage;
    // TonePreview.cpp: a shared preset auditioned before installing it (a bar over every page
    // while it plays), a pack's own view, and links that open the Tones page on a shared tone.
    void SyncTonePreview();
    [[nodiscard]] std::string TonePreviewSignature() const;
    void OpenToneLink(const guitarfx::uiclient::tones::LinkTarget& target);
    void ShowPackDetail(const guitarfx::uiclient::tones::PackDetail& detail);
    Ptr mTonePreviewBar;
    std::string mTonePreviewBarShown;
    bool mToneLinkTaken = false;
    std::optional<guitarfx::uiclient::tones::PackDetail> mPackDetail; // the pack view last shown,
    Ptr mPackDetailOverlay;                                           // its overlay while open,
    std::string mPackDetailSignature;                                 // and what it showed
    std::optional<guitarfx::uiclient::tones::Tone> mTonesShared;      // the tone a link opened the page on
    // Automation.cpp: MIDI and automation, as the web UI's midiLearnMenu.ts and automationPanel.ts.
    /// A control's right-click MIDI Learn menu, for the address it drives.
    void OpenMidiLearnMenu(const std::string& address, const std::string& label, el::point at);
    /// The same, for an effect parameter (nothing for one a mapping cannot reach), and for the
    /// global input and output knobs ("input", "output").
    void OpenNodeParamMidiMenu(const std::string& nodeType, const std::string& paramKey, el::point at);
    void OpenGlobalKnobMidiMenu(const std::string& knob, el::point at);
    /// The MIDI & Automation dialog: Mappings, Keyboard and MIDI Log.
    void OpenMidiAutomation();
    [[nodiscard]] Ptr BuildMidiAutomationBody();
    void OpenAutomationTargetPicker(const std::string& slotId);
    /// After every automation change: announces a finished learn, redraws the dialog.
    void SyncAutomation();
    /// Passes a key on to the engine's keyboard mappings when it is mapped, or a key learn waits.
    bool RouteAutomationKey(const el::key_info& k);
    bool mMidiDialogOpen = false;
    int mMidiDialogTab = 0; // Mappings, Keyboard, MIDI Log
    SlotPtr mMidiDialogBody;
    std::string mMenuLearnSlotId, mMenuLearnLabel; // the learn the right-click menu armed
    // IrAlignment.cpp: the IR Cabinet's Alignment section, in its editor (the web UI's irAlignmentPanel.ts).
    struct IrAlignmentPanel;
    /// The section's height for this node: 0 for any other effect, or a cab with no IR.
    [[nodiscard]] static float IrAlignmentHeight(const guitarfx::GraphNode& node);
    [[nodiscard]] Ptr BuildIrAlignment(const guitarfx::GraphNode& node);
    void SyncIrAlignment();
    std::shared_ptr<IrAlignmentPanel> mIrAlignment;

    // LiveSections.cpp: the 3D Spatial panner and the Signal Analyzer, fed live by the engine.
    /// The section for `node`, or null for an effect without one; `height` is what it needs.
    [[nodiscard]] Ptr BuildLiveSection(const guitarfx::GraphNode& node, float& height);
    void SyncLiveSection();
    std::weak_ptr<el::element> mSpatialPanner, mAnalyzerPanel;
    std::string mLiveNodeId;

    /// Pickup Match's custom designs, described from its curve's reply.
    [[nodiscard]] Ptr BuildPickupDesignLines();
    std::vector<std::pair<std::string, std::weak_ptr<Text>>> mPickupDesignLines;

    // DspStatusView.cpp: the effect's and the output's DSP status readouts.
    struct DspStatusView
    {
        const guitarfx::uiclient::DspStatusReadout* readout = nullptr;
        guitarfx::uiclient::DspStatusMeter meter;
        guitarfx::uiclient::NodeAddress address; // where its loudness is measured; no scope: the output
        SlotPtr slot = MakeSlot();
        std::map<guitarfx::uiclient::DspStatusItem, std::weak_ptr<Text>> values;
    };

    void FillDspStatus(const std::shared_ptr<DspStatusView>& view);
    void SetDspStatusItems(const std::shared_ptr<DspStatusView>& view,
                           const std::vector<guitarfx::uiclient::DspStatusItem>& items);
    [[nodiscard]] Ptr BuildDspStatus(const guitarfx::uiclient::DspStatusReadout& readout, const std::string& nodeKey,
                                     const guitarfx::uiclient::NodeAddress& address, bool overlay);
    void SyncDspStatus();
    void OpenOutputStatus(el::rect anchor);
    std::shared_ptr<DspStatusView> mEffectDspStatus, mOutputDspStatus;
    bool mShowDspStatus = false; // the effect's readout, under its header

    // MultiRig.cpp: tabs over the chain, the inline mixer, saved Multi-Rigs.
    [[nodiscard]] bool MultiRigShown() const;
    [[nodiscard]] std::string MixerSignature() const;
    [[nodiscard]] Ptr BuildMixerTabs();
    [[nodiscard]] Ptr BuildInlineMixer();
    void SaveMultiRig();
    [[nodiscard]] Ptr BuildMultiRigList();
    // PresetChooser.cpp: the preset library as the web UI's popover under the preset selector.
    void OpenPresetChooser(el::rect anchor);
    [[nodiscard]] Ptr BuildPresetChooser();
    void RebuildPresetChooser();
    [[nodiscard]] Ptr BuildPresetCard(const guitarfx::uiclient::PresetSummary& preset);
    bool mMixTab = false;        // the Mix tab: the inline mixer in place of the chain
    bool mShowMultiRigs = false; // the preset chooser lists saved Multi-Rigs

    // SoftwareUpdate.cpp: the once-a-session update check and its Update button.
    void SyncUpdateCheck();
    [[nodiscard]] Ptr BuildUpdateButton();
    void OpenUpdateDialog();
    bool mUpdateChecked = false;
    double mUpdateCheckAt = 0.0;
    std::optional<guitarfx::uiclient::AvailableUpdate> mUpdate;

    /// The Simple Cabinet's Match an IR and Export as IR.
    [[nodiscard]] Ptr BuildCabTools(const guitarfx::GraphNode& node);
    void SyncIrExport();
    std::string mPendingIrExport; // the request id of an export as IR still out
    std::weak_ptr<Text> mIrExportStatus;
    std::optional<EqPlotState> mEffectPlot;  // the effect editor's
    std::optional<EqPlotState> mOverlayPlot; // a dialog's (the Global EQ)

    // PadsView.cpp
    [[nodiscard]] Ptr BuildPadsView();
    void AssignSetlistPad(int index); // puts the preset playing on a pad of the active setlist
    void ClearSetlistPad(int index);
    void LeavePads(); // the pads give way to the editor: refill it, and unlight the footer's pads button

    // PresetsPage.cpp
    Ptr BuildPresetListContent();
    void RebuildPresetList();
    void LoadWithConfirm(const std::string& presetId);
    /// Runs `proceed` (a switch to another preset), asking first when the one playing has unsaved
    /// changes, as the web UI does for every switch it makes: `what` names it ("Load Clean Amp").
    void ConfirmDiscardThen(const std::string& what, std::function<void()> proceed);
    /// The preset selector's previous and next, asking first about unsaved changes.
    void StepPresetWithConfirm(int direction);
    /// Plays a slot of the active setlist (a pad, a setlist row), asking first about unsaved
    /// changes when that loads another preset.
    void SelectSetlistSlot(int index);
    void DeleteWithConfirm(const std::string& presetId);
    void OpenPresetRowMenu(const guitarfx::uiclient::PresetSummary& preset);
    void OpenFolderMenu(const std::string& folderId);
    void SelectPresetBrowserFolder(const std::string& folderId);
    [[nodiscard]] Ptr BuildFolderColumn();
    [[nodiscard]] Ptr BuildSetlistsView(Ptr tabs);
    /// A menu or prompt opened from another menu's item, once that menu has closed.
    void PostMenu(const std::string& title, std::vector<MenuItem> items);
    void PostPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                    std::function<void(const std::string&)> onSubmit);

    // TonesPage.cpp: tone sharing, as the web UI's Tones tab
    enum class TonesTab
    {
        Presets,
        Packs,
        Installed,
        Mine // My Content: the account's own presets and packs
    };
    Ptr BuildTonesPage();
    // SectionNav.cpp
    [[nodiscard]] Ptr WithSectionNav(SectionNav nav, Ptr content);
    /// Says where publishing is (Soundshed Guitar), with a way to My Content.
    void OpenPublishInfo(bool pack = false);
    [[nodiscard]] std::string TonesPageSignature() const;
    /// Asks the service for the open tab's tones: page 1 afresh, or with `more` the next page.
    void LoadTones(bool more);
    void OpenToneAccount(el::rect anchor);
    void OpenPackDetail(const guitarfx::uiclient::tones::Tone& pack);
    void ShareTone(const guitarfx::uiclient::tones::Tone& tone);
    [[nodiscard]] std::shared_ptr<artist::image> RemoteImage(const std::string& url);

    // JamPage.cpp: the Jam tab, as the web UI's (ui-components/panels/jam-panel.html): its
    // sections along the top, then the one open. PracticeToolPage.cpp: the Practice Tool section.
    enum class JamSection
    {
        Search,
        Favorites,
        Scales,
        Riffs,
        Practice
    };
    [[nodiscard]] bool JamShown() const; // the feature is on
    [[nodiscard]] std::vector<JamSection> JamSections() const;
    Ptr BuildJamPage();
    [[nodiscard]] std::string JamPageSignature() const;
    /// Every tick: the Practice Tool's playhead and readouts, its sends that wait on a pause.
    void SyncJam();
    [[nodiscard]] Ptr BuildRiffSection();
    [[nodiscard]] Ptr BuildPracticeTool(float width);
    [[nodiscard]] std::string PracticeToolSignature() const;
    void OpenPracticeProjects(el::rect anchor);
    void OpenPracticeLoopMenu(const std::string& loopId, el::rect anchor);
    void OpenPracticeEq();
    void AddPracticeLink(const std::string& projectId);
    /// An audio file dropped on the Practice Tool loads as its track.
    bool DropPracticeTrack(const std::vector<std::string>& paths);
    [[nodiscard]] Ptr BuildBackingTracks(float width);
    // ScaleGuidePage.cpp: the Scales section, the Scale Guide drawn from the engine's view model
    // with the web UI's own settings (uiclient/ScaleGuide.h), and its LISTEN.
    [[nodiscard]] Ptr BuildScaleGuide(float width);
    [[nodiscard]] std::string ScaleGuideSignature() const;
    const nlohmann::json& ScaleGuideView();
    void ChangeScaleGuide(const std::function<void(guitarfx::uiclient::scaleguide::Settings&)>& edit);
    /// Steps a progression, the next chord led from the shape showing (or `fromFrets`).
    void ScaleGuideStep(int index, std::vector<int> fromFrets = {});
    /// Every tick: LISTEN on while the guide shows with it on, and the readings it brings.
    void SyncScaleGuideListen();
    [[nodiscard]] std::vector<int> ScaleGuideHeard() const;
    [[nodiscard]] bool ScaleGuidePlaying() const;
    nlohmann::json mScaleGuideView;
    std::string mScaleGuideViewKey;
    int mScaleGuideStep = 0;
    std::vector<int> mScaleGuidePrevious; // the shape to lead the next chord from, sent once
    std::weak_ptr<ScaleNeck> mScaleNeck;
    std::weak_ptr<Text> mScaleGuideLive;
    std::string mScaleGuideReadout;
    bool mScaleGuideListening = false, mScaleGuideChroma = false;
    std::uint64_t mScaleGuideReadings = 0;
    guitarfx::uiclient::scaleguide::NoteFollower mScaleGuideFollower;
    guitarfx::uiclient::scaleguide::StrumGate mScaleGuideStrums;
    int mScaleGuideRound = 1, mScaleGuideClean = 0;
    // The YouTube player is a web page: it shows in a web view of its own (nanoq/src/WebView.h)
    // placed over an empty slot in the layout, or, where there is none, in the browser. It keeps
    // playing on other pages, with a bar over them to stop it.
    [[nodiscard]] Ptr BuildJamPlayer(float width);
    [[nodiscard]] nanoq::WebView* JamWebView();
    void CloseJamPlayer();
    void SyncJamWebViews();
    void SyncJamPlayerBar();
    std::optional<guitarfx::uiclient::jam::Video> mJamVideo; // what the player holds
    std::unique_ptr<nanoq::WebView> mJamPlayerView;
    bool mJamWebViewUnavailable = false;
    bool mJamPlayerAudible = false; // the player was making sound at the last tick
    std::weak_ptr<WebViewSlot> mJamPlayerSlot;
    Ptr mJamPlayerBar;
    std::string mJamPlayerBarShown;
    void RunJamSearch();
    void ToggleJamFavorite(const guitarfx::uiclient::jam::Video& video);
    void PlayJamVideo(const guitarfx::uiclient::jam::Video& video);
    JamSection mJamSection = JamSection::Search;
    std::string mJamQuery;
    std::vector<guitarfx::uiclient::jam::Video> mJamResults;
    bool mJamLoading = false;
    bool mJamSearched = false; // the first visit's search, or the cache, has been tried
    int mJamGeneration = 0;    // answers to an older search are dropped
    std::string mJamError;
    std::shared_ptr<el::basic_input_box> mJamSearchBox;
    Ptr mJamSearchElement;
    std::weak_ptr<el::port_base> mJamScroll; // where the open section is scrolled to
    std::unique_ptr<guitarfx::uiclient::PracticeTool> mPractice;
    std::string mJamSignature;
    double mPracticeDeletedAt = 0.0; // when the loop shown in the undo banner was deleted

    // ResourceBrowser.cpp: the model and IR chooser, as the web UI's resource browser
    void OpenResourceBrowser(const std::string& nodeId, const std::string& resourceType, int resourceIndex,
                             std::function<void(const std::string& resourceId)> onPick = {},
                             const std::string& title = {}, const std::string& category = {});
    /// A library row's menu: details, rename, recategorise, copy the path, delete.
    void OpenLibraryResourceMenu(const std::string& resourceType, const std::string& resourceId, el::rect anchor);
    /// A library entry's name, category and tags (the web UI's row edit dialog).
    void EditLibraryResource(const std::string& resourceType, const std::string& resourceId, el::rect anchor);
    /// Asks before deleting a library entry; one in use is refused, saying where.
    void ConfirmDeleteLibraryResource(const std::string& resourceType, const std::string& resourceId, el::rect anchor);
    /// A one-line prompt on the drop-down layer, so it stacks over a dialog.
    void OpenInlinePrompt(el::rect anchor, const std::string& title, const std::string& initial,
                          std::function<void(const std::string&)> onSubmit);

    // SettingsPage.cpp
    enum class SettingsSection
    {
        General,
        Audio,
        Dsp,
        Features,
        Help
    };
    SettingsSection mSettingsSection = SettingsSection::General; // the section Settings shows
    Ptr BuildSettingsPage();

    // Dialogs.cpp
    void ApplyElementsTheme();
    void ShowOverlay(Ptr panel, float width, float height);
    /// A dialog in the shared frame (ui/dialogs/DialogChrome.h), shown as the overlay. The height is
    /// clamped to the window, and the body scrolls when it is cut short.
    void ShowDialog(DialogParts parts);
    /// A footer button for ShowDialog: Primary for the action that confirms, Plain for Cancel.
    [[nodiscard]] std::shared_ptr<Button> DialogButton(const std::string& label, std::function<void()> onClick,
                                                       bool primary);
    void CloseOverlay();
    /// Esc or a tap outside: the dialog's own close (a Cancel that undoes a preview), else closing.
    void CancelOverlay();

    // FilePicker.cpp
    /// The file dialog drawn in the view, for a host with none of its own (Linux, where a kiosk
    /// has no desktop): the whole window, over any dialog that asked for it, and made for touch.
    struct FilePickerState;
    void OpenFilePicker(FileDialogRequest request);
    void CloseFilePicker(const guitarfx::BrowseFileResult& result);
    /// Takes a request from the host; keeps an open picker's list of disks up to date.
    void TickFilePicker();
    void FilePickerGoTo(std::filesystem::path folder);
    void FilePickerConfirm();
    void FillFilePickerPlaces();

    /// Takes a layer (dialog, dropdown, toast, bar) out of the view once the current event is done,
    /// keeping the keyboard focus on the layer that has it.
    void RemoveLayer(Ptr layer);
    void OpenMenu(const std::string& title, std::vector<MenuItem> items);
    void OpenPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                    std::function<void(const std::string&)> onSubmit);
    // RiffDialogs.cpp
    /// The riff capture dialog; `arm` starts counting in at once (the footer's Rec), and
    /// `appendRiffId` saves the take as another take of that riff (the library's Edit).
    void OpenRiffCapture(bool arm, const std::string& appendRiffId = {});
    void OpenRiffLibrary();

    // LooperStrip.cpp
    [[nodiscard]] bool LooperShown() const; // the feature is on and the app is the standalone one
    void ToggleLooper();
    [[nodiscard]] std::string LooperShape() const;
    Ptr BuildLooperStrip();
    /// `labelled`: each spelt out beside its icon, as the full footer does when it is wide.
    std::vector<Ptr> BuildCaptureButtons(float size, bool labelled = false);
    void SyncLooper();
    void SyncLooperLive();
    void RepaintLooperLive();
    void TickLooper();
    void OpenLooperLayers();
    void OpenLooperLoops();
    void OpenLooperOptions();

    void OpenConfirm(const std::string& title, const std::string& detail, const std::string& confirmLabel,
                     std::function<void()> onYes);
    /// Adds the chosen effect after `afterNodeId`, or, with `replaceNodeId`, puts it in that
    /// node's place.
    void OpenEffectPicker(const std::string& afterNodeId, const std::string& replaceNodeId = {});
    /// Adds the chosen effect on this edge of the graph.
    void OpenEffectPickerOnEdge(const guitarfx::GraphEdge& edge);
    void ExpectAddedNode();
    void ShowToast(const std::string& title, const std::string& detail, bool isError);
    [[nodiscard]] std::vector<const guitarfx::uiclient::EffectTypeInfo*> OfferedEffects() const;

    // Instruments.cpp
    void OpenDemoClips();
    [[nodiscard]] const guitarfx::uiclient::DemoClip* SelectedDemoClip() const;
    [[nodiscard]] std::vector<const std::vector<guitarfx::uiclient::DemoClip>*> DemoClipLists() const;
    void OpenTuner();
    void OpenMetronome();
    void OpenControls();
    void OpenGlobalEq(); // EqDialog.cpp
    /// Removes a node from the chain, or collapses the split it opens or closes (Keys.cpp): the
    /// Delete key and a card's cross.
    bool RemoveChainNode(const std::string& nodeId);
    /// The toolbar's Edit: the save dialog over the preset playing (Dialogs.cpp).
    void EditActivePreset();
    /// Imports a preset archive: `path`, or one the user picks when it is empty (Menus.cpp).
    void ImportPresetArchive(const std::string& path);

    // Menus.cpp
    void OpenPresetMenu(std::optional<el::rect> anchor = std::nullopt);
    void OpenNodeMenu(const std::string& nodeId);
    void RenameNodeWithPrompt(const std::string& nodeId);
    void SaveActivePreset(bool asNew);
    /// The save dialog: name, category, description, folder and tags (Dialogs.cpp).
    void OpenPresetDetails(bool asNew);
    [[nodiscard]] std::string MoveTarget(const std::string& nodeId, int direction) const;
    [[nodiscard]] std::string LastNodeBeforeOutput() const;

    UiSession& mSession;
    guitarfx::uiclient::ClientState& mState;
    Theme& mTheme;
    Icons mIcons;
    Images mImages;
    /// What every region moves in place, synced each tick; a binding goes with its widget.
    Bindings mLive{mSession};

    Ptr mRoot;
    std::shared_ptr<KeyRouter> mKeys;
    std::shared_ptr<OnScreenKeyboard> mKeyboard; // null unless the device needs one
    /// What a screen reader sees (AccessBridge.h): made at the first tick, once the view has its
    /// window.
    std::unique_ptr<AccessBridge> mAccess;
    bool mAccessTried = false;
    /// The last click picked a chain card: Space bypasses it and Delete removes it, as when
    /// a card has focus in the web UI. Any other click clears it.
    bool mChainFocused = false;
    SlotPtr mFrameSlot, mEffectSlot;
    SlotPtr mBgSlot, mRailSlot, mTopSlot, mSceneSlot, mChainSlot, mPageSlot, mTransportSlot;

    Page mPage = Page::Chain;
    bool mWide = false;               // the full layout is in force
    bool mControlsOpen = false;       // the compact layout's Controls strip is out
    el::extent mLastSize{0.0f, 0.0f}; // the regions sized to the window are rebuilt when it changes
    bool mPlayStageShown = false;     // the full layout's page slot holds the chain and editor
    float mChainAreaHeight = 0.0f;    // the full layout's chain, as tall as its lines
    float mChainAreaLimit = 0.0f;     // how tall it may grow, once the bar under it is dragged
    float mChainDragStart = -1.0f;    // the chain's room when that drag began
    // Riff capture's settings, kept while the app runs (RiffDialogs.cpp).
    double mRiffTempo = 0.0; // 0 until the dialog first opens, then the metronome's tempo
    int mRiffCountIn = 1;
    bool mRiffClick = false;
    // The looper strip over the footer (LooperStrip.cpp).
    SlotPtr mLooperSlot;
    bool mLooperOpen = false;
    bool mLooperButtonShown = false;
    std::string mLooperShape;
    std::shared_ptr<class Waveform> mLooperWave;
    std::shared_ptr<Button> mLooperMain;
    std::shared_ptr<Text> mLooperStatus, mLooperTime, mLooperBar;
    bool mLibraryExportAll = false; // Settings > Library: the whole library, not only what presets use
    int mChainIcons = -1;           // the chain as icon tiles (1) or cards (0); -1 follows the setting
    bool mChainDragIcons = false;   // which it was when that drag began
    guitarfx::uiclient::PresetTrail mPresetTrail;
    std::shared_ptr<guitarfx::uiclient::TapTempo> mTapTempo;
    bool mWrapChain = true; // the web UI's signalPathWrapCompact, on unless turned off

    // The Tones page: what it shows, and what the service last answered.
    TonesTab mTonesTab = TonesTab::Presets;
    std::string mTonesQuery, mTonesTag;
    int mTonesPage = 1;
    bool mTonesMore = false; // another page of presets to load, or of packs to step to
    bool mTonesLoading = false;
    bool mTonesLoaded = false; // the open tab has been asked for since it was opened
    int mTonesGeneration = 0;  // answers to an older request are dropped
    std::string mTonesError;
    std::vector<guitarfx::uiclient::tones::Tone> mToneItems, mTonePacks, mToneHomePacks;
    std::vector<guitarfx::uiclient::tones::Tone> mToneMine, mToneMinePacks; // My Content
    bool mTonesList = false;                                                // List rather than Cards
    std::shared_ptr<el::basic_input_box> mTonesSearchBox;
    Ptr mTonesSearchElement;
    int mToneListener = 0;
    std::string mTonesSignature;
    RemoteImages mRemoteImages;
    std::unique_ptr<LayoutImages> mLayoutImages; // the custom layouts' pictures

    guitarfx::uiclient::ChainHistory mHistory;
    std::string mHistoryContext;      // presetId::sceneId the stacks describe; a change re-seeds them
    double mHistoryCommitAt = 0.0;    // when to compare the chain again, 0 for no pending edit
    double mHistorySettleUntil = 0.0; // the engine's echo of a restore is not a new edit until then
    std::string mSelectedNode;
    bool mShowAdvanced = false;
    bool mShowIrList = false; // the IR Cabinet's list between its slots, for every cabinet
    unsigned mDirty = kAll;
    bool mShowingStarting = false;
    std::string mAppliedTheme;
    std::uint32_t mChromeCaption = 0xffffffffu; // the caption colour last applied, none yet
    std::string mChainSignature;
    guitarfx::uiclient::SignalFlow mSignalFlow; // what lights the cables, updated per tick
    std::shared_ptr<chain::HitMap> mHitMap;     // where the chain's cards and cables are, for drops
    guitarfx::uiclient::PresetQuery mQuery;
    std::vector<std::string> mPresetTags;                // the tag chips that are on
    std::string mSeenPresetFolder;                       // the engine's folder choice last followed
    el::rect mFolderAnchor;                              // the details dialog's folder button, for its drop-down
    std::shared_ptr<std::function<void()>> mDetailsTags; // the details dialog's tag chips, refilled in place
    std::shared_ptr<el::basic_input_box> mSearchBox;     // kept across rebuilds, with what was typed
    Ptr mSearchElement;
    SlotPtr mPresetListSlot;
    SlotPtr mChooserSlot; // the preset chooser's content, rebuilt as the library changes
    bool mChooserOpen = false;
    bool mChooserSetlistsOpen = false;                   // its Setlists section unfolded
    std::shared_ptr<el::basic_input_box> mFolderNameBox; // its New Folder Name, kept across rebuilds
    Ptr mFolderNameElement;
    std::string mPadsEditingId;                        // the setlist the pads' bank editor is open on
    std::optional<std::string> mPadsEditAfterCreate;   // Add bank: edit the new one, not this one, once made
    std::shared_ptr<el::basic_input_box> mPadsNameBox; // the bank editor's name, kept across rebuilds
    Ptr mPadsNameElement;

    std::string mEffectSignature;
    std::string mSceneSignature;
    std::string mTopSignature;

    Ptr mOverlay;
    Ptr mDropdown; // a drop-down list: a layer of its own, so one opened from a dialog stacks over it
    Ptr mToast;
    Ptr mFilePickerLayer; // over everything: a dialog may be what asked for the file
    std::shared_ptr<FilePickerState> mFilePicker;
    std::filesystem::path mLastPickedFolder; // where the next picker opens
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    std::function<void()> mOnOverlayClosed; // runs when the overlay closes
    std::function<void()> mOverlayCancel;   // what Esc and a tap outside do to the dialog: its own Cancel
    std::vector<guitarfx::uiclient::Subscription> mInstrumentSubs; // an open dialog's live feeds
    guitarfx::uiclient::Subscription mImportWatch;                 // a preset archive import, until it answers
    std::string mPickerCategory;
    std::optional<guitarfx::GraphEdge> mPickerEdge; // the open picker adds on this edge, not after a node
    el::rect mEditorArea;                           // where the full layout's editor is: the FX Library opens over it
    Ptr mDragGhost;                                 // the FX Library's effect being dragged, by the pointer
    guitarfx::uiclient::EffectPresentation mPresentation;

    // After adding an effect, the node that appears next is selected and shown.
    std::set<std::string> mNodesBeforeAdd;
    double mSelectAddedUntil = 0.0;

    std::vector<guitarfx::uiclient::Subscription> mSubscriptions;
};
} // namespace nanoq::ui
