// MultibandSection.cpp - the multi-band effects' section of the effect editor (the web UI's
// signalPath/paramsPanel/multibandPanel.ts): the Bands plot (MultibandPlot), fed with the node's
// spectrum in and out and its band meters, its crossover drags written as the knobs write them.

#include "ui/shell/Shell.h"

namespace nanoq::ui
{
Ptr Shell::BuildMultibandPlot(const guitarfx::GraphNode& node)
{
    const auto kind = MultibandPlot::KindOf(node.type);

    if (!kind)
    {
        mMultibandPlot.reset();
        return nullptr;
    }

    auto plot = std::make_shared<MultibandPlot>(mTheme, *kind);
    // The address, feed keys, reads and writes an EQ plot of the node would use.
    mMultibandSource = NodeEqPlotSource(node);
    plot->read = mMultibandSource.read;
    plot->write = mMultibandSource.write;
    mMultibandPlot = plot;
    return plot;
}

bool Shell::SyncMultibandFeed()
{
    if (!mMultibandPlot || !PlayShown() || mMultibandSource.feedKeys.empty())
    {
        return false;
    }

    mSession.Client().GetAnalysis().WatchSpectrum(mMultibandSource.address);

    // A spectrum that stopped arriving is cleared rather than left frozen, as the web UI does.
    const auto& spectra = mState.analysis.spectra;
    const double now = UiSession::Now();
    const guitarfx::uiclient::NodeSpectrum* spectrum = nullptr;

    for (const auto& key : mMultibandSource.feedKeys)
    {
        if (const auto it = spectra.find(key); it != spectra.end() && now - it->second.receivedAt < 0.5)
        {
            spectrum = &it->second;
        }
    }

    if (mMultibandPlot->SetSpectrum(spectrum, now))
    {
        mSession.Repaint(*mMultibandPlot);
    }

    return true;
}
} // namespace nanoq::ui
