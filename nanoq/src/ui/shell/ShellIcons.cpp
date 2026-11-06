// The icons the web UI writes inline in its markup (ui-components/*.html) rather than keeping in
// images/icons: the compact rail, the preset bar, the footer and the chain's wrap switch. Copied
// path for path, so the shell draws the same glyphs; the stroke widths are the ones its CSS sets.

#include "ui/shell/ShellIcons.h"

namespace nanoq::ui
{
namespace
{
constexpr const char* kStroke = R"( fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" )";

std::string Svg(const char* viewBox, float strokeWidth, const char* body)
{
    return std::string("<svg viewBox=\"") + viewBox + "\"" + kStroke + "stroke-width=\"" + std::to_string(strokeWidth) +
           "\">" + body + "</svg>";
}
} // namespace

void AddShellIcons(Icons& icons)
{
    // The rail (header-icon-bar.html).
    icons.Add(
        "ui-chain",
        Svg("0 0 24 24", 1.8f,
            R"(<rect x="2" y="9" width="5" height="6" rx="1.2"/><rect x="9.5" y="7" width="5" height="10" rx="1.2"/>)"
            R"(<rect x="17" y="9" width="5" height="6" rx="1.2"/><path d="M7 12h2.5M14.5 12H17"/>)"));
    icons.Add("ui-effect", Svg("0 0 24 24", 1.8f,
                               R"(<circle cx="12" cy="13.5" r="7"/><path d="M12 13.5 16 9.5"/>)"
                               R"(<path d="M12 3v1.5M4.2 6.2l1.1 1.1M19.8 6.2l-1.1 1.1"/>)"));
    icons.Add(
        "ui-tones",
        Svg("0 0 24 24", 1.8f,
            R"(<path d="M9 17.5V5.5l11-2v12"/><circle cx="6.5" cy="17.5" r="2.5"/><circle cx="17.5" cy="15.5" r="2.5"/>)"));
    icons.Add("ui-jam",
              Svg("0 0 24 24", 1.8f,
                  R"(<path d="M4 17v-5a8 8 0 0 1 16 0v5"/><rect x="3" y="14" width="4.5" height="6.5" rx="1.5"/>)"
                  R"(<rect x="16.5" y="14" width="4.5" height="6.5" rx="1.5"/>)"));
    icons.Add(
        "ui-pause",
        R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="6" y="5" width="4" height="14" rx="1"/><rect x="14" y="5" width="4" height="14" rx="1"/></svg>)");
    icons.Add(
        "ui-jump-back",
        R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="5" y="5" width="2.5" height="14" rx="1"/><polygon points="19 5 19 19 9 12 19 5"/></svg>)");
    icons.Add("ui-clock", Svg("0 0 24 24", 1.8f, R"(<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>)"));
    icons.Add("ui-download",
              Svg("0 0 24 24", 2.0f, R"(<path d="M12 4v11"/><path d="m7 10 5 5 5-5"/><path d="M5 20h14"/>)"));
    icons.Add("ui-share",
              Svg("0 0 24 24", 2.0f,
                  R"(<circle cx="18" cy="5" r="3"/><circle cx="6" cy="12" r="3"/><circle cx="18" cy="19" r="3"/>)"
                  R"(<path d="M8.6 10.7l6.8-4.4"/><path d="M8.6 13.3l6.8 4.4"/>)"));
    icons.Add("ui-eye",
              Svg("0 0 24 24", 2.0f,
                  R"(<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"));
    icons.Add(
        "ui-trash",
        Svg("0 0 24 24", 2.0f,
            R"(<path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/><path d="M10 11v6M14 11v6"/>)"));
    icons.Add(
        "ui-folder",
        Svg("0 0 24 24", 1.8f,
            R"(<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z"/>)"));
    icons.Add("ui-file", Svg("0 0 24 24", 1.8f, R"(<path d="M6 3h8l5 5v13H6z"/><polyline points="14 3 14 8 19 8"/>)"));
    icons.Add("ui-up", Svg("0 0 24 24", 2.0f, R"(<polyline points="6 15 12 9 18 15"/>)"));
    icons.Add("ui-chevron-right", Svg("0 0 24 24", 2.0f, R"(<polyline points="9 6 15 12 9 18"/>)"));
    icons.Add("ui-presets", Svg("0 0 24 24", 1.8f, R"(<path d="M4 6h16M4 12h16M4 18h10"/>)"));
    icons.Add(
        "ui-gear",
        Svg("0 0 24 24", 1.5f,
            R"(<path d="M12 15.5a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7z"/>)"
            R"(<path d="M19.4 15a1.7 1.7 0 0 0 .34 1.86l.03.03a2 2 0 0 1-2.83 2.83l-.03-.03A1.7 1.7 0 0 0 15 19.4a1.7 1.7 0 0 0-1 1.53V21a2 2 0 0 1-4 0v-.07a1.7 1.7 0 0 0-1-1.53 1.7 1.7 0 0 0-1.86.34l-.03.03a2 2 0 1 1-2.83-2.83l.03-.03A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-1.53-1H3a2 2 0 0 1 0-4h.07a1.7 1.7 0 0 0 1.53-1 1.7 1.7 0 0 0-.34-1.86l-.03-.03a2 2 0 1 1 2.83-2.83l.03.03A1.7 1.7 0 0 0 9 4.6 1.7 1.7 0 0 0 10 3.07V3a2 2 0 0 1 4 0v.07a1.7 1.7 0 0 0 1 1.53 1.7 1.7 0 0 0 1.86-.34l.03-.03a2 2 0 1 1 2.83 2.83l-.03.03A1.7 1.7 0 0 0 19.4 9c0 .46.17.9.46 1.24.3.33.74.53 1.21.56H21a2 2 0 0 1 0 4h-.07a1.7 1.7 0 0 0-1.53 1z"/>)"));

    // The preset bar (preset-selector-row.html, preset-toolbar-row.html).
    icons.Add("ui-prev",
              Svg("0 0 24 24", 2.0f, R"(<line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>)"));
    icons.Add("ui-next",
              Svg("0 0 24 24", 2.0f, R"(<line x1="5" y1="12" x2="19" y2="12"/><polyline points="12 5 19 12 12 19"/>)"));
    icons.Add(
        "ui-new",
        Svg("0 0 24 24", 2.0f,
            R"(<rect x="3" y="3" width="18" height="18" rx="4"/><line x1="12" y1="7" x2="12" y2="17"/><line x1="7" y1="12" x2="17" y2="12"/>)"));
    icons.Add("ui-edit", Svg("0 0 24 24", 2.0f,
                             R"(<path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/>)"
                             R"(<path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z"/>)"));
    icons.Add("ui-save", Svg("0 0 24 24", 2.0f,
                             R"(<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/>)"
                             R"(<polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/>)"));
    icons.Add(
        "ui-random",
        Svg("0 0 24 24", 2.0f,
            R"(<path d="M16 3h5v5"/><path d="M4 20L21 3"/><path d="M21 16v5h-5"/><path d="M15 15l6 6"/><path d="M4 4l5 5"/>)"));
    icons.Add(
        "ui-more",
        R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><circle cx="5" cy="12" r="1.75"/><circle cx="12" cy="12" r="1.75"/><circle cx="19" cy="12" r="1.75"/></svg>)");
    icons.Add("ui-caret", Svg("0 0 24 24", 2.0f, R"(<polyline points="6 9 12 15 18 9"/>)"));

    // The footer (footer-bar.html).
    icons.Add(
        "ui-pads",
        Svg("0 0 24 24", 1.8f,
            R"(<rect x="3.5" y="3.5" width="7" height="7" rx="1.5"/><rect x="13.5" y="3.5" width="7" height="7" rx="1.5"/>)"
            R"(<rect x="3.5" y="13.5" width="7" height="7" rx="1.5"/><rect x="13.5" y="13.5" width="7" height="7" rx="1.5"/>)"));
    icons.Add("ui-tuner", Svg("0 0 24 24", 1.8f, R"(<path d="M9 3v7a3 3 0 0 0 6 0V3"/><path d="M12 13v8"/>)"));
    // A DIN socket: MIDI & Automation (#footer-midi-btn).
    icons.Add(
        "ui-midi",
        Svg("0 0 24 24", 1.8f,
            R"(<circle cx="12" cy="12" r="9"/><path d="M10 20v-2h4v2"/>)"
            R"(<circle cx="7.5" cy="10.5" r="1.15" fill="currentColor"/><circle cx="9.6" cy="7.6" r="1.15" fill="currentColor"/>)"
            R"(<circle cx="12" cy="6.6" r="1.15" fill="currentColor"/><circle cx="14.4" cy="7.6" r="1.15" fill="currentColor"/>)"
            R"(<circle cx="16.5" cy="10.5" r="1.15" fill="currentColor"/>)"));
    icons.Add("ui-undo", Svg("0 0 24 24", 1.8f, R"(<path d="M9 7 4 12l5 5"/><path d="M4 12h9a6 6 0 0 1 0 12h-1"/>)"));
    icons.Add("ui-redo", Svg("0 0 24 24", 1.8f, R"(<path d="M15 7l5 5-5 5"/><path d="M20 12h-9a6 6 0 0 0 0 12h1"/>)"));
    icons.Add("ui-swap", Svg("0 0 24 24", 1.8f, R"(<path d="M4 8h13l-3.5-3.5"/><path d="M20 16H7l3.5 3.5"/>)"));
    icons.Add(
        "ui-metronome",
        Svg("0 0 24 24", 1.8f,
            R"(<path d="M9 3.5h6L19 20.5H5L9 3.5Z"/><path d="M10 7.5h4"/><path d="M12 8.5v7"/><path d="M12 10.5l2.75-1.75"/>)"));
    icons.Add(
        "ui-play",
        R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><polygon points="5 3 19 12 5 21 5 3"/></svg>)");
    icons.Add(
        "ui-stop",
        R"(<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="6" y="6" width="12" height="12" rx="1.5"/></svg>)");
    icons.Add("ui-record", Svg("0 0 24 24", 1.8f, R"(<circle cx="12" cy="12" r="7"/>)"));
    icons.Add("ui-loop", Svg("0 0 24 24", 1.8f, R"(<path d="M18.5 12a6.5 6.5 0 1 1-2-4.7"/><path d="M17 3.5v4h-4"/>)"));
    icons.Add(
        "ui-io",
        Svg("0 0 24 24", 1.8f,
            R"(<path d="M4 7h10M4 17h6M18 7h2M14 17h6"/><circle cx="16" cy="7" r="2"/><circle cx="12" cy="17" r="2"/>)"));
    icons.Add("ui-mute",
              Svg("0 0 24 24", 1.8f, R"(<path d="M11 5 6 9H3v6h3l5 4V5z"/><path d="M22 9l-6 6M16 9l6 6"/>)"));
    icons.Add("ui-speaker",
              Svg("0 0 24 24", 1.8f, R"(<path d="M11 5 6 9H3v6h3l5 4V5z"/><path d="M15.5 8.5a5 5 0 0 1 0 7"/>)"));

    // The input modes' routing (inputModeIcons.ts): the inputs at the left (filled when used),
    // the chain as a box, the two sides leaving at the right.
    {
        const std::string open = R"(<svg viewBox="0 0 30 20" fill="none" stroke="currentColor" stroke-width="1.7" )"
                                 R"(stroke-linecap="round" stroke-linejoin="round">)";
        const std::string in1 = R"(<circle cx="3" cy="5" r="2.2" fill="currentColor" stroke="none"/>)";
        const std::string in2 = R"(<circle cx="3" cy="15" r="2.2" fill="currentColor" stroke="none"/>)";
        const std::string in1Unused = R"(<circle cx="3" cy="5" r="1.7" stroke-width="1.3"/>)";
        const std::string in2Unused = R"(<circle cx="3" cy="15" r="1.7" stroke-width="1.3"/>)";
        const std::string from1 = R"(<path d="M5.2 5c3.3 0 3 5 6.3 5"/>)";
        const std::string from2 = R"(<path d="M5.2 15c3.3 0 3-5 6.3-5"/>)";
        const std::string monoChain = R"(<rect x="11.5" y="6.5" width="7" height="7" rx="1.8"/>)"
                                      R"(<path d="M18.5 10c2.6 0 2.6-5 5.2-5H27M18.5 10c2.6 0 2.6 5 5.2 5H27"/>)";
        const std::string lanes = R"(<path d="M5.2 5h6.3M18.5 5H27M5.2 15h6.3M18.5 15H27"/>)";
        const std::string stereoChain = R"(<rect x="11.5" y="1.5" width="7" height="17" rx="2"/>)";
        const std::string dualChains = R"(<rect x="11.5" y="1.5" width="7" height="7" rx="1.8"/>)"
                                       R"(<rect x="11.5" y="11.5" width="7" height="7" rx="1.8"/>)";
        const std::string close = "</svg>";
        icons.Add("input-mono1", open + in1 + in2Unused + from1 + monoChain + close);
        icons.Add("input-mono2", open + in1Unused + in2 + from2 + monoChain + close);
        icons.Add("input-monoSum", open + in1 + in2 + from1 + from2 + monoChain + close);
        icons.Add("input-stereo", open + in1 + in2 + lanes + stereoChain + close);
        icons.Add("input-dualMono", open + in1 + in2 + lanes + dualChains + close);
    }

    // The preset chooser's tools (preset-library-popover.html).
    // The model browser's row details and pack grouping (resourceBrowser.ts).
    icons.Add("ui-info", Svg("0 0 24 24", 2.0f,
                             R"(<circle cx="12" cy="12" r="9"/><line x1="12" y1="16" x2="12" y2="11"/>)"
                             R"(<line x1="12" y1="8" x2="12.01" y2="8"/>)"));
    icons.Add("ui-box",
              Svg("0 0 24 24", 2.0f,
                  R"(<path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 )"
                  R"(1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><polyline points="3.27 6.96 12 12.01 20.73 )"
                  R"(6.96"/><line x1="12" y1="22.08" x2="12" y2="12"/>)"));
    icons.Add("ui-upload",
              Svg("0 0 24 24", 2.0f,
                  R"(<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 5 17 10"/>)"
                  R"(<line x1="12" y1="5" x2="12" y2="19"/>)"));
    icons.Add("ui-folder-add",
              Svg("0 0 24 24", 2.0f,
                  R"(<path d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>)"
                  R"(<line x1="12" y1="10" x2="12" y2="16"/><line x1="9" y1="13" x2="15" y2="13"/>)"));
    icons.Add(
        "ui-folder-rename",
        Svg("0 0 24 24", 2.0f,
            R"(<path d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v3"/><path d="M14 20h7"/><path d="M16.5 6.5l3 3"/>)"
            R"(<path d="M13 17l6.5-6.5a2.12 2.12 0 0 0-3-3L10 14v3z"/>)"));

    // The performance pads' save button (performancePads.ts).
    icons.Add("ui-pad-save", Svg("0 0 24 24", 2.0f, R"(<path d="M6 4h9l3 3v13H6zM9 4v5h6M9 16h6"/>)"));

    // The chain (signal-path-bar.html).
    icons.Add(
        "ui-wrap",
        Svg("0 0 16 16", 1.5f,
            R"(<path d="M2 3.5h12M2 8h9.25a2.25 2.25 0 0 1 0 4.5H7.5M9.25 10.75 7.5 12.5l1.75 1.75M2 12.5h3"/>)"));
}
} // namespace nanoq::ui
