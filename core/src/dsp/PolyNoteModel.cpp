#include "dsp/PolyNoteModel.h"

#include <array>

namespace guitarfx::poly_note_model
{
namespace
{
// The trained weights and BuildModel(), generated: see the file's own header.
#include "dsp/PolyNoteModelData.inc"
} // namespace

PolyNoteModel Model()
{
    return BuildModel();
}
} // namespace guitarfx::poly_note_model
