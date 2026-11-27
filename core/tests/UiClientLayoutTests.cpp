/**
 * UiClientLayoutTests.cpp - Custom effect layouts as the native UIs read and draw them
 * (core/src/uiclient/EffectLayouts.h): the layout-compat fixtures and the factory layout parse,
 * the preference rules choose as the web UI's layoutPreferences.ts does, the scale and the
 * image knobs follow layoutScaling.ts and layoutKnobImages.ts, layout colours read as the
 * renderer's do, and the client keeps the library and image list the engine sends.
 */

#include "uiclient/EffectLayouts.h"
#include "uiclient/UiClient.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace ui = guitarfx::uiclient;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

bool Near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

const std::filesystem::path kUiDir = GUITARFX_UI_DIR;

nlohmann::json ReadJson(const std::filesystem::path& path)
{
    std::ifstream in(path);

    if (!in)
    {
        std::cerr << "FAIL: cannot read " << path.string() << std::endl;
        ++gFailures;
        return nlohmann::json::object();
    }

    return nlohmann::json::parse(in, nullptr, false);
}

/// A fixture's layout: stored ones hold it in `data`, archives in `data.layout`.
ui::EffectLayout FixtureLayout(const std::string& name)
{
    const auto fixture = ReadJson(kUiDir / "tests" / "fixtures" / "layout-compat" / (name + ".json"));
    const auto& data = fixture.value("data", nlohmann::json::object());
    return ui::ParseEffectLayout(fixture.value("kind", "") == "archive" ? data.value("layout", nlohmann::json::object())
                                                                        : data);
}

void TestFixturesParse()
{
    {
        const auto layout = FixtureLayout("export-1-6-0");
        Expect(layout.name == "Blue Echo" && layout.effectType == "delay_digital", "1.6.0 export: name and type");
        Expect(Near(layout.dimensions.width, 480) && Near(layout.dimensions.height, 320), "1.6.0 export: canvas");
        Expect(layout.backgrounds.size() == 2 && layout.backgrounds[0].type == "color" &&
                   layout.backgrounds[1].type == "image" && Near(layout.backgrounds[1].opacity, 0.85) &&
                   Near(layout.backgrounds[1].offsetY, 8),
               "1.6.0 export: a colour layer under an image layer");
        Expect(layout.controls.size() == 5, "1.6.0 export: five controls");
        Expect(layout.controls[0].style.knobStyle == "custom" &&
                   layout.controls[0].style.knobImageId == "chicken-head_1771000000002",
               "1.6.0 export: a custom knob face");
        Expect(layout.controls[1].style.labelPosition == "bottom" && layout.controls[1].style.knobStyle == "pedal",
               "1.6.0 export: a pedal knob labelled underneath");
        Expect(layout.controls[2].labelOverride == "Blend" && layout.controls[2].type == "slider",
               "1.6.0 export: a renamed slider");
        Expect(!layout.controls[3].style.showValue, "1.6.0 export: a toggle with no value");
        Expect(layout.overlays.size() == 2 && layout.overlays[0].visibilityMode == "enabled" &&
                   !layout.overlays[0].toggleBypassOnClick && layout.overlays[1].toggleBypassOnClick &&
                   Near(layout.overlays[1].backgroundOpacity, 0.0),
               "1.6.0 export: an LED and a footswitch");
        Expect(layout.textLabels.size() == 2 && layout.textLabels[0].bold && Near(layout.textLabels[0].fontSize, 22) &&
                   !layout.textLabels[1].bold,
               "1.6.0 export: a bold title and a plain subtitle");
        Expect(layout.containerTheme == "dark" && !layout.useDefaultControls, "1.6.0 export: theme, own controls");
        const auto ids = ui::LayoutImageIds(layout);
        Expect(ids.size() == 2 && ids[0] == "pedal-face_1771000000001" && ids[1] == "chicken-head_1771000000002",
               "1.6.0 export: the images it draws on, in order");
    }
    {
        const auto layout = FixtureLayout("background-image-modes");
        Expect(layout.backgrounds.size() == 6, "background modes: every layer kept");
        Expect(layout.backgrounds[0].size == "tile" && layout.backgrounds[1].size == "custom" &&
                   layout.backgrounds[1].scale && Near(*layout.backgrounds[1].scale, 0.5) &&
                   Near(layout.backgrounds[1].offsetX, 128) && Near(layout.backgrounds[1].offsetY, -16) &&
                   Near(layout.backgrounds[1].opacity, 0.6),
               "background modes: tile, then a custom scale with offsets");
        Expect(layout.backgrounds[2].size == "contain" && layout.backgrounds[3].size == "stretch" &&
                   layout.backgrounds[4].size == "cover",
               "background modes: contain, stretch, and cover by default");
        Expect(ui::LayoutImageIds(layout).size() == 4, "background modes: four distinct images");
    }
    {
        const auto layout = FixtureLayout("control-size-and-styles");
        Expect(layout.controls.size() == 10, "sizes: every control kept, the removed parameter's too");
        Expect(layout.controls[0].size && Near(layout.controls[0].size->width, 96), "sizes: a wrapper width");
        Expect(layout.controls[3].style.hideLabel && layout.controls[3].type == "toggle",
               "sizes: a toggle with its label hidden");
        Expect(layout.controls[4].style.labelPosition == "none" && !layout.controls[4].style.showValue,
               "sizes: no label, no value");
        Expect(layout.controls[9].type == "dropdown", "sizes: a dropdown, drawn as a knob");
        Expect(layout.backgrounds.size() == 1 && layout.backgrounds[0].type == "gradient",
               "sizes: a gradient background");
        Expect(layout.controlLabelColor == "#ccddee", "sizes: the layout's label colour");
    }
    {
        const auto layout = FixtureLayout("backdrop-offset-scale");
        Expect(layout.useDefaultControls, "backdrop: draws the standard controls");
        Expect(Near(layout.defaultControlsOffset.x, 24) && Near(layout.defaultControlsOffset.y, 96) &&
                   Near(layout.defaultControlsScale.x, 1.25) && Near(layout.defaultControlsScale.y, 0.9),
               "backdrop: their offset and scale");
        Expect(layout.overlays.size() == 2 && layout.overlays[0].visibilityMode == "bypassed" &&
                   Near(layout.overlays[1].backgroundOpacity, 0.25) && Near(layout.overlays[1].borderWidth, 1),
               "backdrop: an overlay shown when bypassed, and one with the defaults");
    }
    {
        const auto layout = FixtureLayout("resource-selector");
        Expect(layout.controls.size() == 4, "resources: four controls");
        Expect(ui::IsLayoutResourceControl(layout.controls[0]) && ui::IsLayoutResourceControl(layout.controls[1]) &&
                   !ui::IsLayoutResourceControl(layout.controls[3]),
               "resources: by key, by binding type, and a knob");
        const auto slot = ui::LayoutControlResourceSlot(layout.controls[1]);
        Expect(slot && slot->resourceType == "ir" && slot->resourceIndex == 1,
               "resources: a slot's type and index from its key");
        Expect(layout.controls[1].labelOverride == "Cabinet", "resources: a renamed slot");
    }
    {
        const auto layout = FixtureLayout("image-assets-data-urls");
        Expect(layout.controls.size() == 1 && ui::LayoutImageIds(layout).size() == 2,
               "embedded images: ids read, data ignored");
    }
    {
        // The factory layout every install ships: a backdrop for the standard controls with a
        // footswitch hotspot.
        const auto archive = ReadJson(kUiDir / "assets" / "layouts" / "chaos-engine" / "layout.json");
        const auto layout = ui::ParseEffectLayout(archive.value("layout", nlohmann::json::object()));
        Expect(layout.useDefaultControls && Near(layout.defaultControlsScale.x, 0.7) &&
                   Near(layout.dimensions.width, 696),
               "factory layout: a backdrop at 0.7");
        Expect(layout.overlays.size() == 1 && layout.overlays[0].toggleBypassOnClick,
               "factory layout: its bypass hotspot");
        Expect(layout.backgrounds.size() == 1 && layout.backgrounds[0].size == "custom",
               "factory layout: one image background");
    }
}

void TestLenientParsing()
{
    const auto layout = ui::ParseEffectLayout(nlohmann::json::parse(R"({
        "dimensions": {"width": "640", "height": -5},
        "backgrounds": [{"layerIndex": 1, "type": "color", "value": "#111"}, "junk", {"layerIndex": 0, "type": "color", "value": "#222"}],
        "controls": [{"paramKey": "a", "style": {"knobStyle": "x;y", "knobImage": {"mode": "filmstrip", "imageId": "f", "frames": 2.5}}},
                     {"type": "knob"}, 7],
        "overlays": [{"style": {"backgroundOpacity": "0.9", "imageFit": "zoom"}}],
        "scaling": {"mode": "zoom", "maxScale": -1}
    })"));
    Expect(Near(layout.dimensions.width, 640) && Near(layout.dimensions.height, 280),
           "a numeric string and a bad height");
    Expect(layout.backgrounds.size() == 2 && layout.backgrounds[0].value == "#222", "layers sorted, junk dropped");
    Expect(layout.controls.size() == 1 && layout.controls[0].style.knobStyle == "default",
           "a control without a key dropped; an odd style");
    Expect(ui::UsableKnobImage(layout.controls[0].style) == nullptr,
           "a filmstrip without a whole frame count is not usable");
    Expect(Near(layout.overlays[0].backgroundOpacity, 0.25) && layout.overlays[0].imageFit == "contain",
           "an opacity that is not a number, and an unknown fit");
    Expect(layout.scaling && layout.scaling->mode == "fit-width" && !layout.scaling->maxScale,
           "an unknown mode and scale ignored");
}

ui::LayoutLibrary MakeLibrary()
{
    auto entry = [](const std::string& id, const std::string& name) {
        return nlohmann::json{{"layoutId", id},
                              {"layout", {{"name", name}, {"effectType", "amp_nam"}, {"layoutId", id}}}};
    };
    return ui::ParseLayoutLibrary(
        nlohmann::json{{"byEffectType",
                        {{"amp_nam", {entry("tweed", "Tweed Face"), entry("modern", "Modern Face")}},
                         {"amp_nam_blend::blend-1", {entry("blend-face", "Blend")}}}},
                       {"defaults", {{"amp_nam", "tweed"}}},
                       {"images", nlohmann::json::array()}});
}

nlohmann::json Rule(const std::string& key, const std::string& scope, const std::string& layoutId,
                    const std::string& extraKey = {}, const std::string& extra = {})
{
    nlohmann::json rule{{"lookupKey", key}, {"scope", scope}, {"layoutId", layoutId}};

    if (!extraKey.empty())
    {
        rule[extraKey] = extra;
    }

    return rule;
}

ui::LayoutSelection Resolve(const ui::LayoutLibrary& library, const nlohmann::json& rules, const std::string& type,
                            const std::string& blendId = {}, const std::string& matchText = {},
                            const std::string& presetId = {})
{
    const nlohmann::json settings{{ui::kLayoutPreferencesSetting, rules}};
    return ui::ResolveLayoutSelection(&library, ui::ParseLayoutPreferenceRules(settings),
                                      {type, blendId, matchText, presetId});
}

void TestPreferences()
{
    const auto library = MakeLibrary();
    const auto none = nlohmann::json::array();
    const std::string standard = ui::kStandardLayoutId;

    Expect(ui::LayoutLookupKeys("amp_nam_blend", "blend-1") ==
               std::vector<std::string>{"amp_nam_blend::blend-1", "amp_nam_blend"},
           "the blend key first");
    Expect(ui::LayoutLookupKeys("amp_nam", "") == std::vector<std::string>{"amp_nam"}, "one key without a blend");
    Expect(ui::AvailableLayoutEntries(library, "amp_nam", "").size() == 2, "the entries a node can pick");

    // The FX Library's card: the default layout's thumbnail, a blend's own before its effect's.
    const auto* face = ui::DefaultLibraryLayout(library, "amp_nam");
    Expect(face != nullptr && face->name == "Tweed Face", "the effect's default layout");
    Expect(ui::DefaultLibraryLayout(library, "amp_nam", "blend-9") == face, "a blend without one has its effect's");
    Expect(ui::DefaultLibraryLayout(library, "delay_digital") == nullptr, "and an effect with none, none");
    Expect(ui::ImageDataUrlBytes("data:image/png;base64,iVBORw==") == std::string("\x89PNG"), "a picture's bytes");
    Expect(ui::ImageDataUrlBytes("data:image/svg+xml;base64,PHN2Zz4=").empty() &&
               ui::ImageDataUrlBytes("data:image/png;base64,ab\"c").empty() && ui::ImageDataUrlBytes("").empty(),
           "but not a script's, a broken one's or nothing's");

    auto selection = Resolve(library, none, "amp_nam");
    Expect(selection.layoutId == std::optional<std::string>("tweed") && selection.source == "libraryDefault",
           "the library default when no rules exist");
    selection = Resolve(library, none, "delay_digital");
    Expect(!selection.layoutId && selection.source == "none", "nothing for an effect with neither");

    selection = Resolve(library, nlohmann::json::array({Rule("amp_nam", "effectType", standard)}), "amp_nam");
    Expect(selection.layoutId == std::optional<std::string>(standard) && selection.source == "effectType",
           "an effect-type rule picks the standard controls");

    const auto keywordRules = nlohmann::json::array(
        {Rule("amp_nam", "effectType", standard), Rule("amp_nam", "keyword", "modern", "keyword", "Rectifier")});
    Expect(Resolve(library, keywordRules, "amp_nam", "", "mesa rectifier lead").layoutId ==
               std::optional<std::string>("modern"),
           "a keyword rule (lower-cased) beats the effect-type rule");
    Expect(Resolve(library, keywordRules, "amp_nam", "", "vox ac30").source == "effectType",
           "a keyword that does not match");

    const auto longest = nlohmann::json::array({Rule("amp_nam", "keyword", "tweed", "keyword", "vox"),
                                                Rule("amp_nam", "keyword", "modern", "keyword", "vox ac30")});
    Expect(Resolve(library, longest, "amp_nam", "", "vox ac30 top boost").layoutId ==
               std::optional<std::string>("modern"),
           "the longest keyword wins");
    Expect(Resolve(library, longest, "amp_nam", "", "vox ac15").layoutId == std::optional<std::string>("tweed"),
           "else the shorter");

    const auto presetRules = nlohmann::json::array({Rule("amp_nam", "effectType", "modern"),
                                                    Rule("amp_nam", "keyword", "tweed", "keyword", "rectifier"),
                                                    Rule("amp_nam", "preset", standard, "presetId", "preset-7")});
    Expect(Resolve(library, presetRules, "amp_nam", "", "mesa rectifier", "preset-7").source == "preset",
           "a preset rule first");
    Expect(Resolve(library, presetRules, "amp_nam", "", "mesa rectifier", "preset-8").source == "keyword",
           "another preset");

    const auto blendRules = nlohmann::json::array(
        {Rule("amp_nam_blend", "effectType", standard), Rule("amp_nam_blend::blend-1", "effectType", "blend-face")});
    Expect(Resolve(library, blendRules, "amp_nam_blend", "blend-1").layoutId ==
               std::optional<std::string>("blend-face"),
           "the blend key first");
    Expect(Resolve(library, blendRules, "amp_nam_blend", "blend-2").layoutId == std::optional<std::string>(standard),
           "another blend");

    const auto malformed =
        nlohmann::json::array({Rule("amp_nam", "keyword", "modern"), Rule("", "effectType", "modern"), "x",
                               Rule("amp_nam", "nope", "modern")});
    Expect(Resolve(library, malformed, "amp_nam").source == "libraryDefault", "malformed rules ignored");

    Expect(ui::FindLayoutById(library, "modern", "amp_nam", "") != nullptr &&
               ui::FindLayoutById(library, standard, "amp_nam", "") == nullptr &&
               ui::FindLayoutById(library, "missing", "amp_nam", "") == nullptr,
           "finding a layout by id");

    // A default that names no layout of its key is no default.
    auto stale = library;
    stale.defaults["amp_nam"] = "gone";
    Expect(ui::ResolveLayoutSelection(&stale, {}, {"amp_nam", "", "", ""}).source == "none",
           "a default for a missing layout");
}

void TestScaling()
{
    const auto scale = [](double availableWidth, double availableHeight = 0.0,
                          std::optional<ui::LayoutScaling> scaling = std::nullopt) {
        return ui::ComputeLayoutScale(480, 320, availableWidth, availableHeight, scaling);
    };
    const auto is = [](const ui::LayoutScaleResult& r, double s, bool centred, bool overflows) {
        return Near(r.scale, s) && r.centred == centred && r.overflows == overflows;
    };
    const auto with = [](std::string mode, std::optional<double> minScale, std::optional<double> maxScale,
                         std::string align = "center") {
        return std::optional<ui::LayoutScaling>(
            ui::LayoutScaling{std::move(mode), minScale, maxScale, std::move(align)});
    };

    for (const double width : {480.0, 481.0, 1200.0, 5000.0})
    {
        Expect(is(scale(width, 50), 1, true, false), "no scaling block: own size, centred, never grows");
    }

    Expect(is(scale(240, 10), 0.5, false, false), "no scaling block: shrinks to the width");
    Expect(is(scale(479), 479.0 / 480.0, false, false), "no scaling block: a pixel short");
    Expect(is(scale(960, 0, with("fit-width", std::nullopt, 4.0)), 2, false, false), "grows up to maxScale");
    Expect(is(scale(960, 0, with("fit-width", std::nullopt, 1.5)), 1.5, true, false), "and centres once it stops");
    Expect(is(scale(960, 320, with("fit", std::nullopt, 4.0)), 1, true, false), "fit keeps within the height");
    Expect(is(scale(960, 160, with("fit", std::nullopt, std::nullopt)), 0.5, true, false), "fit shrinks to the height");
    Expect(Near(scale(240, 0, with("fit", std::nullopt, std::nullopt)).scale, 0.5),
           "fit with no height fits the width");
    Expect(is(scale(120, 0, with("fit-width", 0.5, std::nullopt)), 0.5, false, true), "minScale overflows");
    Expect(is(scale(240, 0, with("fixed", std::nullopt, std::nullopt)), 1, false, true), "fixed overflows");
    Expect(is(scale(960, 0, with("fixed", std::nullopt, 3.0)), 1, true, false), "fixed never grows");
    Expect(is(scale(960, 0, with("fit-width", std::nullopt, std::nullopt, "start")), 1, false, false),
           "at the start when asked");
    Expect(Near(scale(96000, 0, with("fit-width", std::nullopt, 1000.0)).scale, 8), "capped at 8");
    Expect(Near(scale(120, 0, with("fit-width", 2.0, 1.0)).scale, 1), "a minScale over maxScale");
}

void TestKnobImages()
{
    Expect(ui::FilmstripFrameCount(128) == 128, "128 frames");

    for (const double odd : {1.0, 0.0, -5.0, 2.5, 5000.0, std::nan("")})
    {
        Expect(ui::FilmstripFrameCount(odd) == 0, "an unusable frame count");
    }

    Expect(ui::FilmstripFrame(0, 101) == 0 && ui::FilmstripFrame(0.5, 101) == 50 && ui::FilmstripFrame(1, 101) == 100 &&
               ui::FilmstripFrame(1.7, 101) == 100 && ui::FilmstripFrame(std::nan(""), 101) == 0,
           "travel onto frames, ends included");

    auto cell = ui::FilmstripCellOf(63, 64, 1);
    Expect(cell.columns == 1 && cell.rows == 64 && cell.column == 0 && cell.row == 63, "a vertical strip's last frame");
    cell = ui::FilmstripCellOf(5, 10, 4);
    Expect(cell.columns == 4 && cell.rows == 3 && cell.column == 1 && cell.row == 1,
           "a sheet: left to right, then down");
    cell = ui::FilmstripCellOf(3, 4, 4);
    Expect(cell.rows == 1 && cell.column == 3, "a horizontal strip is one row");
    cell = ui::FilmstripCellOf(2, 4, 9);
    Expect(cell.columns == 4, "no more columns than frames");

    Expect(Near(ui::KnobFaceAngle(0), -135) && Near(ui::KnobFaceAngle(0.5), 0) && Near(ui::KnobFaceAngle(1), 135),
           "the default sweep");
    Expect(Near(ui::KnobFaceAngle(1, 0, 300), 300) && Near(ui::KnobFaceAngle(0.5, -150, 150), 0), "a face's own sweep");

    ui::LayoutControlStyle style;
    Expect(Near(ui::LayoutKnobDiameter(style), 38) && Near(ui::LayoutSliderLength(style), 80), "the original sizes");
    style.knobDiameter = 900.0;
    style.toggleWidth = 3.0;
    Expect(Near(ui::LayoutKnobDiameter(style), 512) && Near(ui::LayoutToggleSize(style).width, 8) &&
               Near(ui::LayoutToggleSize(style).height, 20),
           "sizes clamped to their ranges");
}

// The layout's own switch, knob picture and light, and the strips a toggle or slider is drawn
// from (layoutControlImages.ts, layoutShadows.ts).
void TestControlImagesAndShadows()
{
    const auto layout = ui::ParseEffectLayout(nlohmann::json::parse(R"({
        "effectType": "x", "dimensions": {"width": 400, "height": 200},
        "knobImage": {"mode": "filmstrip", "imageId": "sheet", "frames": 64},
        "switchImage": {"strip": {"imageId": "sw", "frames": 2, "aspect": 0.5}, "width": 40},
        "controlShadow": {"angle": 765, "elevation": 5, "opacity": 2},
        "controls": [
            {"paramKey": "a", "type": "knob", "position": {"x": 0, "y": 0}},
            {"paramKey": "b", "type": "knob", "position": {"x": 0, "y": 0}, "style": {"knobStyle": "pedal"}},
            {"paramKey": "c", "type": "toggle", "position": {"x": 0, "y": 0}},
            {"paramKey": "d", "type": "toggle", "position": {"x": 0, "y": 0},
             "style": {"toggleImage": {"onImageId": "on", "offImageId": "off"}, "castShadow": false}},
            {"paramKey": "e", "type": "slider", "position": {"x": 0, "y": 0},
             "style": {"sliderLength": 120, "sliderImage": {"strip": {"imageId": "fader", "frames": 32, "aspect": 4}}}},
            {"paramKey": "f", "type": "slider", "position": {"x": 0, "y": 0},
             "style": {"sliderImage": {"strip": {"imageId": "bad", "frames": 1}}}}
        ]})"));

    const auto& c = layout.controls;
    Expect(c.size() == 6, "six controls");
    Expect(ui::UsableKnobImage(c[0].style) != nullptr && c[0].style.knobImage->imageId == "sheet",
           "a default knob takes the layout's picture");
    Expect(ui::UsableKnobImage(c[1].style) == nullptr, "a Pedal knob keeps its own look");
    Expect(c[2].style.toggleStrip && c[2].style.toggleStrip->imageId == "sw" && ui::UsableToggleImage(c[2].style),
           "a plain toggle takes the layout's switch");
    Expect(Near(ui::LayoutToggleSize(c[2].style).width, 40) && Near(ui::LayoutToggleSize(c[2].style).height, 20),
           "the switch's width, and the strip's shape for its height");
    Expect(!c[3].style.toggleStrip && c[3].style.toggleOnImageId == "on" && !c[3].style.castShadow,
           "a toggle with pictures keeps them; castShadow false is read");
    Expect(c[4].style.sliderStrip && c[4].style.sliderStrip->frames == 32, "a slider's strip");
    const auto fader = ui::StripSliderSize(c[4].style, *c[4].style.sliderStrip);
    Expect(fader.vertical && Near(fader.height, 120) && Near(fader.width, 30), "a tall strip runs up its length");
    Expect(!c[5].style.sliderStrip, "a strip of one frame is not usable");

    const auto ids = ui::LayoutImageIds(layout);
    const auto has = [&ids](const char* id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    Expect(has("sheet") && has("sw") && has("fader") && has("on"), "the pictures the controls draw are loaded");

    Expect(layout.controlShadow && Near(layout.controlShadow->angle, 45) && Near(layout.controlShadow->elevation, 15) &&
               Near(layout.controlShadow->opacity, 1),
           "the light's fields are held in range");

    // A 38 px knob lit from the top left at 45 degrees: its shadow falls 38 px down and right.
    ui::LayoutControlShadow light;
    const auto shadow = ui::ControlShadowOf(light, 38, 38, 19);
    double maxX = -1e9, maxY = -1e9;

    for (const auto& p : shadow.hull)
    {
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }

    Expect(Near(shadow.radius, 19) && std::abs(maxX - (19 + 38 * std::sqrt(0.5))) < 1e-6 &&
               std::abs(maxY - (19 + 38 * std::sqrt(0.5))) < 1e-6,
           "the footprint swept away from the light");
    Expect(shadow.fadeTo.x > shadow.fadeFrom.x && shadow.fadeTo.y > shadow.fadeFrom.y, "it fades away from the light");
}

void TestColours()
{
    const auto is = [](const std::optional<ui::LayoutColour>& c, int r, int g, int b, double a) {
        return c && c->r == r && c->g == g && c->b == b && Near(c->a, a);
    };
    Expect(is(ui::ParseLayoutColour("#abc"), 0xaa, 0xbb, 0xcc, 1.0), "#rgb");
    Expect(is(ui::ParseLayoutColour(" #1B2A4A "), 0x1b, 0x2a, 0x4a, 1.0), "#rrggbb, trimmed");
    Expect(is(ui::ParseLayoutColour("#ff000080"), 255, 0, 0, 128.0 / 255.0), "#rrggbbaa");
    Expect(is(ui::ParseLayoutColour("rgba(10, 20, 30, 0.5)"), 10, 20, 30, 0.5), "rgba()");
    Expect(is(ui::ParseLayoutColour("rgb(100% 0 0 / 25%)"), 255, 0, 0, 0.25), "space-separated rgb()");
    Expect(is(ui::ParseLayoutColour("White"), 255, 255, 255, 1.0) &&
               is(ui::ParseLayoutColour("transparent"), 0, 0, 0, 0.0),
           "named colours");
    Expect(!ui::ParseLayoutColour("#12") && !ui::ParseLayoutColour("red; color: blue") &&
               !ui::ParseLayoutColour("url(x)") && !ui::ParseLayoutColour("var(--text)") && !ui::ParseLayoutColour(""),
           "what is not a colour");

    const auto gradient = ui::ParseLayoutPaint("linear-gradient(180deg, #333 0%, #111 100%)");
    Expect(gradient.kind == ui::LayoutPaint::Kind::Linear && Near(gradient.angleDegrees, 180) &&
               gradient.stops.size() == 2 && gradient.stops[0].colour.r == 0x33 && Near(gradient.stops[1].offset, 1.0),
           "a linear gradient");
    const auto spread = ui::ParseLayoutPaint("linear-gradient(to right, red, rgba(0,0,0,0.5), blue)");
    Expect(spread.kind == ui::LayoutPaint::Kind::Linear && Near(spread.angleDegrees, 90) && spread.stops.size() == 3 &&
               Near(spread.stops[1].offset, 0.5) && Near(spread.stops[1].colour.a, 0.5),
           "a side, and stops spread evenly");
    Expect(ui::ParseLayoutPaint("#222").kind == ui::LayoutPaint::Kind::Solid, "a colour paints solid");
    Expect(ui::ParseLayoutPaint("radial-gradient(red, blue)").kind == ui::LayoutPaint::Kind::None &&
               ui::ParseLayoutPaint("linear-gradient(red, url(x))").kind == ui::LayoutPaint::Kind::None,
           "what is not drawn");

    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ui::LinearGradientLine(180, 100, 50, x0, y0, x1, y1);
    Expect(Near(x0, 50) && Near(y0, 0) && Near(x1, 50) && Near(y1, 50), "180deg runs top to bottom");
    ui::LinearGradientLine(90, 100, 50, x0, y0, x1, y1);
    Expect(Near(x0, 0) && Near(x1, 100), "90deg runs left to right");

    ui::EffectLayout layout;
    layout.controlLabelColor = "#102030";
    ui::LayoutControl control;
    Expect(is(ui::LayoutLabelColour(layout, control), 0x10, 0x20, 0x30, 1.0) && !ui::LayoutValueColour(layout, control),
           "the layout's label colour, the theme's value colour");
    control.style.labelColor = "#fff";
    Expect(is(ui::LayoutLabelColour(layout, control), 255, 255, 255, 1.0), "a control's own colour wins");
    control.style.labelFontSize = 200.0;
    layout.controlValueFontSize = 12.0;
    Expect(Near(ui::LayoutLabelFontSize(layout, control), 96) && Near(ui::LayoutValueFontSize(layout, control), 12),
           "text sizes: clamped, and the layout's default");
}

void TestControls()
{
    guitarfx::uiclient::EffectParamInfo toggleParam;
    toggleParam.unit = "toggle";
    toggleParam.name = "Sync";
    ui::LayoutControl control;
    control.paramKey = "sync";
    Expect(ui::ClassifyLayoutControl(control, &toggleParam) == ui::LayoutControlKind::Toggle, "a toggle by its unit");
    control.type = "slider";
    Expect(ui::ClassifyLayoutControl(control, nullptr) == ui::LayoutControlKind::Slider, "a slider");
    control.type = "dropdown";
    Expect(ui::ClassifyLayoutControl(control, nullptr) == ui::LayoutControlKind::Knob, "a dropdown draws as a knob");
    Expect(ui::LayoutControlLabel(control, &toggleParam) == "Sync" &&
               ui::LayoutControlLabel(control, nullptr) == "sync",
           "the label: the parameter's name, else its key");
    control.paramKey = "__resource__:primary:1";
    const auto slot = ui::LayoutControlResourceSlot(control);
    Expect(ui::ClassifyLayoutControl(control, nullptr) == ui::LayoutControlKind::Resource && slot &&
               slot->resourceType == "primary" && slot->resourceIndex == 1,
           "a primary slot");
    control.paramKey = "__resource__:primary:x";
    Expect(!ui::LayoutControlResourceSlot(control), "a slot with no index");

    ui::LayoutOverlay overlay;
    Expect(ui::LayoutOverlayVisible(overlay, true) && ui::LayoutOverlayVisible(overlay, false), "always");
    overlay.visibilityMode = "enabled";
    Expect(ui::LayoutOverlayVisible(overlay, false) && !ui::LayoutOverlayVisible(overlay, true), "while enabled");
    overlay.visibilityMode = "bypassed";
    Expect(!ui::LayoutOverlayVisible(overlay, false) && ui::LayoutOverlayVisible(overlay, true), "while bypassed");
}

void TestClient()
{
    std::vector<std::string> sent;
    ui::UiClient client([&sent](const std::string& json) { sent.push_back(json); });
    int notified = 0;
    auto subscription = client.Subscribe(ui::Topic::EffectLayouts, [&notified] { ++notified; });

    const auto chaos = ReadJson(kUiDir / "assets" / "layouts" / "chaos-engine" / "layout.json")
                           .value("layout", nlohmann::json::object());
    const std::string type = chaos.value("effectType", "");
    nlohmann::json library{
        {"byEffectType", {{type, {{{"layoutId", "factory::chaos"}, {"isFactory", true}, {"layout", chaos}}}}}},
        {"defaults", {{type, "factory::chaos"}}},
        {"images", nlohmann::json::array()}};
    client.Enqueue(nlohmann::json{{"type", "layoutLibraryLoaded"}, {"layoutLibrary", library}}.dump());
    client.DrainPending();
    const auto& state = client.State();
    Expect(state.layouts.loaded && notified == 1 && state.layouts.library.byEffectType.count(type) == 1,
           "the library arrives");

    guitarfx::GraphNode node;
    node.id = "n1";
    node.type = type;
    const auto* layout = ui::ResolveLayoutForNode(state, node);
    Expect(layout != nullptr && layout->useDefaultControls, "a node of its type draws the factory layout");
    node.type = "delay_digital";
    Expect(ui::ResolveLayoutForNode(state, node) == nullptr, "another effect draws the standard controls");
    node.type = type;

    // Choosing the standard controls is a rule, kept in the app setting; choosing it back too.
    auto& mutableState = client.MutableState();
    mutableState.appSettings[ui::kLayoutPreferencesSetting] =
        ui::RulesWithLayoutChoice(state.appSettings, state, node, ui::kStandardLayoutId, "2026-10-27T00:00:00Z");
    Expect(ui::ResolveLayoutForNode(state, node) == nullptr, "the standard controls chosen");
    const auto& rules = state.appSettings[ui::kLayoutPreferencesSetting];
    Expect(rules.size() == 1 && rules[0].value("scope", "") == "effectType" && rules[0].value("lookupKey", "") == type,
           "one effect-type rule");
    mutableState.appSettings[ui::kLayoutPreferencesSetting] =
        ui::RulesWithLayoutChoice(state.appSettings, state, node, "factory::chaos", "2026-10-27T00:00:01Z");
    Expect(ui::ResolveLayoutForNode(state, node) != nullptr &&
               state.appSettings[ui::kLayoutPreferencesSetting].size() == 1,
           "the layout chosen again, replacing the rule");

    client.RequestLayoutImages();
    client.RequestLayoutImages();
    Expect(sent.size() == 1 && nlohmann::json::parse(sent[0]).value("type", "") == "requestLayoutImages",
           "the images asked for once");

    client.Enqueue(
        R"({"type":"layoutImagesLoaded","images":[{"imageId":"chaos-engine_17726216187144984","fileName":"a.png","url":"layout-images/id/x?v=5","version":"5","width":696,"height":600},{"fileName":"no id"}]})");
    client.DrainPending();
    Expect(state.layouts.imagesLoaded && state.layouts.images.size() == 1 &&
               state.layouts.images.begin()->second.width == 696 && state.layouts.images.begin()->second.version == "5",
           "the image list arrives");

    // A reloaded library is asked about again.
    client.Enqueue(nlohmann::json{{"type", "layoutLibraryLoaded"}, {"layoutLibrary", library}}.dump());
    client.DrainPending();
    client.RequestLayoutImages();
    Expect(sent.size() == 2 && notified == 3, "the images asked for again after a reload");
}
} // namespace

int main()
{
    TestFixturesParse();
    TestLenientParsing();
    TestPreferences();
    TestScaling();
    TestKnobImages();
    TestControlImagesAndShadows();
    TestColours();
    TestControls();
    TestClient();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " failure(s)" << std::endl;
        return 1;
    }

    std::cout << "UiClientLayoutTests: all passed" << std::endl;
    return 0;
}
