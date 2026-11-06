#pragma once
// A probe for how much drawing the UI does, for performance work: with
// SOUNDSHED_NANOQ_DRAWSTATS set to a file path, the frame is wrapped in it and each second it
// appends one line: how many paints there were, how much of the window they covered (the
// hosts repaint the bounding box of everything invalidated since the last paint, so two small
// changes at opposite corners cost a wide band), and the time spent drawing. Unset, nothing
// is wrapped and it costs nothing. SOUNDSHED_NANOQ_DRAWRECTS set as well writes each paint's
// rectangle to the same path with ".rects" added, to see what is being invalidated.
//
// On a GPU backend (Direct2D) the times are small; on a CPU one (Quartz on iOS) the area is what
// costs, so the area is the number to bring down.

#include <elements.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nanoq::ui
{
namespace el = cycfi::elements;

class DrawProbe final : public el::proxy_base
{
  public:
    DrawProbe(el::element_ptr subject, std::string path) : mSubject(std::move(subject)), mPath(std::move(path))
    {
    }

    /// The path from SOUNDSHED_NANOQ_DRAWSTATS, or empty when the probe is off.
    [[nodiscard]] static std::string PathFromEnvironment()
    {
        const char* path = std::getenv("SOUNDSHED_NANOQ_DRAWSTATS");
        return path != nullptr ? std::string(path) : std::string();
    }

    el::element& subject() override
    {
        return *mSubject;
    }

    const el::element& subject() const override
    {
        return *mSubject;
    }

    void draw(const el::context& ctx) override
    {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        proxy_base::draw(ctx);
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();

        const auto dirty = cycfi::artist::intersection(ctx.bounds, ctx.canvas.clip_extent());
        const double window = static_cast<double>(ctx.bounds.width()) * ctx.bounds.height();

        if (std::getenv("SOUNDSHED_NANOQ_DRAWRECTS") != nullptr)
        {
            if (std::FILE* f = std::fopen((mPath + ".rects").c_str(), "a"))
            {
                std::fprintf(f, "%.0f %.0f %.0f %.0f\n", dirty.left, dirty.top, dirty.right, dirty.bottom);
                std::fclose(f);
            }
        }

        ++mPaints;
        mArea += window > 0.0 ? static_cast<double>(dirty.width()) * dirty.height() / window : 0.0;
        mMs += ms;
        mMaxMs = std::max(mMaxMs, ms);

        const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();

        if (mSince == 0.0)
        {
            mSince = now;
        }

        if (now - mSince >= 1.0)
        {
            if (std::FILE* f = std::fopen(mPath.c_str(), "a"))
            {
                // paints/s, windows repainted per second, ms drawing per second, slowest paint.
                std::fprintf(f, "paints %d  area %.2f windows/s  draw %.1f ms/s  max %.1f ms\n", mPaints,
                             mArea / (now - mSince), mMs / (now - mSince), mMaxMs);
                std::fclose(f);
            }

            mSince = now;
            mPaints = 0;
            mArea = mMs = mMaxMs = 0.0;
        }
    }

  private:
    el::element_ptr mSubject;
    std::string mPath;
    int mPaints = 0;
    double mArea = 0.0, mMs = 0.0, mMaxMs = 0.0, mSince = 0.0;
};
} // namespace nanoq::ui
