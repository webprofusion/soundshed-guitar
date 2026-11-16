#pragma once

#include <span>

namespace guitarfx
{
/// A trained note model: what PolyNoteDetector (dsp/PolyNoteDetector.h) reads for each note, and
/// the network that judges it. The weights are rows of `inputs` values, one row per output.
struct PolyNoteModel
{
    struct Dense
    {
        int inputs = 0;
        int outputs = 0;
        const float* weights = nullptr;
        const float* bias = nullptr;
    };

    std::span<const int> channels; ///< which of PolySpectrum's grids are read
    std::span<const float> ratios; ///< the partials read, as multiples of the note's frequency
    std::span<const int> offsets;  ///< the bins read round each, in thirds of a semitone
    std::span<const int> context;  ///< the frames read, as frames back from the newest
    /// How far under the frame's level a partial still reads as something, in bels (20 dB each):
    /// the levels are scaled so that this far under is 0 and the level itself is 1.
    float range = 3.0f;
    std::span<const Dense> body;     ///< the note's own reading: each layer followed by a ReLU
    Dense summary;                   ///< what a note tells its neighbours (tanh); no outputs for none
    std::span<const int> neighbours; ///< the neighbours a note hears from, in semitones
    Dense mix;                       ///< the neighbours' summaries, then the note's own reading (ReLU)
    Dense head;                      ///< two logits: sounding, started
};

namespace poly_note_model
{
/// The note model the product ships (PolyNoteModel.cpp, from the generated PolyNoteModelData.inc).
/// It points at static data, so it is free to copy and valid for the life of the program.
[[nodiscard]] PolyNoteModel Model();
} // namespace poly_note_model
} // namespace guitarfx
