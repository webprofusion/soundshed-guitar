// Writes static instances of the web UI's variable Inter for Soundshed Guitar Nano.
//
// JUCE 8 cannot choose a variable font's weight. Given Inter's variable font, DirectWrite
// hands JUCE the family's first named instance, Thin, while HarfBuzz lays the text out with
// the default instance's (Regular's) advances: hairline glyphs, loosely spaced. So Nano ships
// one static font per weight instead, cut from the same file the web UI uses. HarfBuzz's own
// instancer does the cutting (glyf/gvar, metrics, kerning and all), from the copy of HarfBuzz
// that JUCE already carries, so nothing is downloaded. That copy has no repacker (the graph/
// folder), so gen-nano-fonts.mjs builds the subsetter with the repacker replaced by a stub
// that stops the run: a table that needs repacking would otherwise come out empty.
//
//   make-inter-instances <Inter-VariableFont_opsz,wght.ttf> <output folder>
//
// Built and run by tools/gen-nano-fonts.mjs.

#include "hb-subset.h"
#include "hb.h"

#include <cstdio>
#include <string>

namespace
{
struct Instance
{
    const char* fileName;
    float weight;
};

// The weights Nano draws with: body text, emphasis, and headings and values.
constexpr Instance kInstances[] = {
    { "Inter-Regular.ttf", 400.0f },
    { "Inter-Medium.ttf", 500.0f },
    { "Inter-SemiBold.ttf", 600.0f },
};

// Inter's text optical size. Nano's text runs 10-20 px, where the display cut is too tight.
constexpr float kOpticalSize = 14.0f;

bool writeBlob (hb_blob_t* blob, const std::string& path)
{
    unsigned int length = 0;
    const char* data = hb_blob_get_data (blob, &length);

    if (data == nullptr || length == 0)
        return false;

    std::FILE* file = std::fopen (path.c_str(), "wb");

    if (file == nullptr)
        return false;

    const bool ok = std::fwrite (data, 1, length, file) == length;
    return std::fclose (file) == 0 && ok;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf (stderr, "usage: make-inter-instances <variable font> <output folder>\n");
        return 2;
    }

    hb_blob_t* source = hb_blob_create_from_file_or_fail (argv[1]);

    if (source == nullptr)
    {
        std::fprintf (stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    hb_face_t* face = hb_face_create (source, 0);
    int failures = 0;

    for (const auto& instance : kInstances)
    {
        hb_subset_input_t* input = hb_subset_input_create_or_fail();

        // Every character, with the layout features HarfBuzz shapes by default, which are the
        // ones JUCE uses. Keeping the optional ones too (stylistic sets, tabular figures...)
        // overflows GPOS, and needs the repacker JUCE's copy of HarfBuzz leaves out.
        hb_face_collect_unicodes (face, hb_subset_input_unicode_set (input));

        const bool pinned = hb_subset_input_pin_axis_location (input, face, HB_TAG ('w', 'g', 'h', 't'), instance.weight)
                            && hb_subset_input_pin_axis_location (input, face, HB_TAG ('o', 'p', 's', 'z'), kOpticalSize);
        hb_face_t* result = pinned ? hb_subset_or_fail (face, input) : nullptr;
        hb_subset_input_destroy (input);

        const std::string path = std::string (argv[2]) + "/" + instance.fileName;

        if (result == nullptr)
        {
            std::fprintf (stderr, "could not instance %s\n", instance.fileName);
            ++failures;
            continue;
        }

        hb_blob_t* blob = hb_face_reference_blob (result);

        if (writeBlob (blob, path))
        {
            std::printf ("%s (%u bytes)\n", path.c_str(), hb_blob_get_length (blob));
        }
        else
        {
            std::fprintf (stderr, "could not write %s\n", path.c_str());
            ++failures;
        }

        hb_blob_destroy (blob);
        hb_face_destroy (result);
    }

    hb_face_destroy (face);
    hb_blob_destroy (source);
    return failures == 0 ? 0 : 1;
}
