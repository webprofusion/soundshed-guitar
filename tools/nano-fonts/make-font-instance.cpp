// Writes one static instance of a variable font for Soundshed Nano Q.
//
// The native UI ships one static font per weight, cut from the same variable fonts the web UI
// uses: Inter for every theme, and the theme fonts (Oswald, Archivo) for the
// themes that set them. It began with JUCE 8, which cannot choose a variable font's weight -
// given Inter's variable font, DirectWrite handed it the family's first named instance, Thin,
// while HarfBuzz laid the text out with the default instance's advances. Static cuts also keep
// the platforms' font matching simple: one file per weight, matched by its OS/2 weight class.
//
// HarfBuzz's own instancer does the cutting (glyf/gvar, metrics, kerning and all), from the copy
// of HarfBuzz that JUCE already carries, so nothing is downloaded. That copy has no repacker
// (the graph/ folder), so gen-nano-fonts.mjs builds the subsetter with the repacker replaced by
// a stub that stops the run: a table that needs repacking would otherwise come out empty.
//
//   make-font-instance <variable font> <output file> <axis>=<value>...
//
// for example `make-font-instance Inter.ttf Inter-Medium.ttf wght=500 opsz=14`. Every axis the
// font has must be pinned. Built and run by tools/gen-nano-fonts.mjs.

#include "hb-subset.h"
#include "hb.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
bool writeBlob(hb_blob_t* blob, const char* path)
{
    unsigned int length = 0;
    const char* data = hb_blob_get_data(blob, &length);

    if (data == nullptr || length == 0)
    {
        return false;
    }

    std::FILE* file = std::fopen(path, "wb");

    if (file == nullptr)
    {
        return false;
    }

    const bool ok = std::fwrite(data, 1, length, file) == length;
    return std::fclose(file) == 0 && ok;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: make-font-instance <variable font> <output file> <axis>=<value>...\n");
        return 2;
    }

    hb_blob_t* source = hb_blob_create_from_file_or_fail(argv[1]);

    if (source == nullptr)
    {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    hb_face_t* face = hb_face_create(source, 0);
    hb_subset_input_t* input = hb_subset_input_create_or_fail();

    // Every character, with the layout features HarfBuzz shapes by default. Keeping the optional
    // ones too (stylistic sets, tabular figures...) overflows Inter's GPOS, and needs the
    // repacker JUCE's copy of HarfBuzz leaves out.
    hb_face_collect_unicodes(face, hb_subset_input_unicode_set(input));

    bool pinned = true;

    for (int i = 3; i < argc && pinned; ++i)
    {
        const std::string setting = argv[i];
        const auto equals = setting.find('=');

        if (equals != 4)
        {
            std::fprintf(stderr, "axis setting %s is not <four-letter tag>=<value>\n", argv[i]);
            pinned = false;
            break;
        }

        const hb_tag_t tag = hb_tag_from_string(setting.c_str(), 4);
        const float value = std::strtof(setting.c_str() + 5, nullptr);
        pinned = hb_subset_input_pin_axis_location(input, face, tag, value);

        if (!pinned)
        {
            std::fprintf(stderr, "%s has no axis %.4s\n", argv[1], setting.c_str());
        }
    }

    hb_face_t* result = pinned ? hb_subset_or_fail(face, input) : nullptr;
    hb_subset_input_destroy(input);
    int status = 0;

    if (result == nullptr)
    {
        std::fprintf(stderr, "could not instance %s\n", argv[2]);
        status = 1;
    }
    else
    {
        hb_blob_t* blob = hb_face_reference_blob(result);

        if (writeBlob(blob, argv[2]))
        {
            std::printf("%s (%u bytes)\n", argv[2], hb_blob_get_length(blob));
        }
        else
        {
            std::fprintf(stderr, "could not write %s\n", argv[2]);
            status = 1;
        }

        hb_blob_destroy(blob);
        hb_face_destroy(result);
    }

    hb_face_destroy(face);
    hb_blob_destroy(source);
    return status;
}
