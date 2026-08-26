// Tests for the shared plugin path/format rules.
//
// Plugin formats disagree per-platform about whether a plugin is a file or a
// directory, and file dialogs disagree about whether a directory can even be
// selected — so a user may arrive at a plugin either by picking the bundle or by
// picking a payload file buried inside it. These tests pin the normalization
// that makes both routes land on the same stored path, and the one format table
// that the dialog, the loader and the resource library all read from.

#include <iostream>
#include <string>

#include "resources/PluginPathUtils.h"

using guitarfx::pluginpath::HasPluginBundleSuffix;
using guitarfx::pluginpath::PluginBrowseFilters;
using guitarfx::pluginpath::PluginFormat;
using guitarfx::pluginpath::PluginFormatFromPath;
using guitarfx::pluginpath::PluginFormatId;
using guitarfx::pluginpath::ResolvePluginBundlePath;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
  if (!condition)
  {
    std::cerr << "  FAILED: " << what << "\n";
    ++gFailures;
  }
}

void ExpectPath(const std::filesystem::path& actual, const std::filesystem::path& expected,
                const std::string& what)
{
  if (actual.generic_string() != expected.generic_string())
  {
    std::cerr << "  FAILED: " << what << "\n    expected: " << expected.generic_string()
              << "\n    actual:   " << actual.generic_string() << "\n";
    ++gFailures;
  }
}

void ExpectFormat(PluginFormat actual, PluginFormat expected, const std::string& what)
{
  if (actual != expected)
  {
    std::cerr << "  FAILED: " << what << "\n    expected: " << PluginFormatId(expected)
              << "\n    actual:   " << PluginFormatId(actual) << "\n";
    ++gFailures;
  }
}

void TestBundleSuffixDetection()
{
  Expect(HasPluginBundleSuffix("/plugins/Foo.vst3"), "a .vst3 path is a bundle");
  Expect(HasPluginBundleSuffix("/plugins/Foo.lv2"), "a .lv2 path is a bundle");
  Expect(HasPluginBundleSuffix("/plugins/Foo.component"), "a .component path is a bundle");
  Expect(HasPluginBundleSuffix("/plugins/FOO.VST3"), "suffix matching is case-insensitive");
  Expect(HasPluginBundleSuffix("/plugins/Foo.vst3/"), "a trailing separator does not hide the suffix");

  Expect(!HasPluginBundleSuffix("/plugins/Foo.so"), "a bare .so is not a bundle");
  Expect(!HasPluginBundleSuffix("/plugins/Foo.dll"), "a bare .dll is not a bundle");
  Expect(!HasPluginBundleSuffix("/plugins/VST3"), "a folder merely named VST3 is not a bundle");
  Expect(!HasPluginBundleSuffix("/plugins/Foo.vst3backup"), "a longer suffix does not match");
  Expect(!HasPluginBundleSuffix(""), "an empty path is not a bundle");

  // ".vst" means VST2 and must not swallow ".vst3".
  Expect(HasPluginBundleSuffix("/plugins/Foo.vst"), "a .vst path is recognized (VST2)");

  // Formats this host cannot load are still recognized, so a selection reaches the
  // loader and earns a specific "not supported" message.
  Expect(HasPluginBundleSuffix("/plugins/Foo.clap"), "a .clap path is recognized");
  Expect(HasPluginBundleSuffix("/plugins/Foo.aaxplugin"), "an .aaxplugin path is recognized");
}

void TestResolvesPayloadToBundleRoot()
{
  // Linux VST3: the payload is a .so several levels inside the bundle.
  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.vst3/Contents/x86_64-linux/Foo.so"),
             "/plugins/Foo.vst3",
             "a Linux VST3 payload resolves to the bundle root");

  // LV2 anywhere: users pick manifest.ttl or the inner binary.
  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.lv2/manifest.ttl"),
             "/plugins/Foo.lv2",
             "an LV2 manifest resolves to the bundle directory");

  // macOS Audio Unit: the executable has no extension at all.
  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.component/Contents/MacOS/Foo"),
             "/plugins/Foo.component",
             "an Audio Unit executable resolves to the bundle root");

  // Unsupported formats normalize too, so the loader sees ".clap" and can name it.
  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.clap/Contents/x86_64-linux/Foo.so"),
             "/plugins/Foo.clap",
             "a CLAP bundle payload resolves to the bundle root");

  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.vst3"),
             "/plugins/Foo.vst3",
             "a bundle root resolves to itself");
}

void TestSameNameAncestorWins()
{
  // The Windows case: the bundle root and its payload share a name, and the root
  // is what the VST3 spec and the JUCE scanner expect. Picking either must store
  // the same path.
  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.vst3/Contents/x86_64-win/Foo.vst3"),
             "/plugins/Foo.vst3",
             "a Windows VST3 inner binary resolves to the outer bundle, not itself");
}

void TestContainerFolderDoesNotSwallowPlugin()
{
  // Climbing stops at the nearest match unless an ancestor repeats its name, so a
  // folder that merely ends in a bundle suffix cannot claim the plugin inside it.
  ExpectPath(ResolvePluginBundlePath("/plugins/Container.vst3/Foo.vst3"),
             "/plugins/Container.vst3/Foo.vst3",
             "a differently named bundle ancestor does not swallow the plugin");

  ExpectPath(ResolvePluginBundlePath("/Audio.vst/Plugins/Foo.vst3"),
             "/Audio.vst/Plugins/Foo.vst3",
             "an unrelated suffixed folder high in the path is ignored");
}

void TestNonBundlePathsArePreserved()
{
  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.dll"),
             "/plugins/Foo.dll",
             "a bare VST2 DLL is left alone so the loader can report it");

  ExpectPath(ResolvePluginBundlePath("/plugins/somewhere/else.txt"),
             "/plugins/somewhere/else.txt",
             "an unrelated file is left alone");

  ExpectPath(ResolvePluginBundlePath(""), "", "an empty path stays empty");

  ExpectPath(ResolvePluginBundlePath("/plugins/Foo.vst3/"),
             "/plugins/Foo.vst3",
             "a trailing separator is trimmed");
}

void TestFormatDetection()
{
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.vst3"), PluginFormat::VST3, "a .vst3 bundle is VST3");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.vst3/Contents/x86_64-linux/Foo.so"),
               PluginFormat::VST3, "a payload is classified by its bundle, not its own extension");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.lv2/manifest.ttl"), PluginFormat::LV2,
               "an LV2 manifest is LV2");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.component"), PluginFormat::AudioUnit,
               "a .component bundle is an Audio Unit");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.appex"), PluginFormat::AudioUnit,
               "an .appex bundle is an Audio Unit");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.clap"), PluginFormat::CLAP, "a .clap bundle is CLAP");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.aaxplugin"), PluginFormat::AAX,
               "an .aaxplugin bundle is AAX");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.dll"), PluginFormat::VST2, "a bare .dll is VST2");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.vst"), PluginFormat::VST2, "a .vst bundle is VST2");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.so"), PluginFormat::Unknown,
               "a bare .so outside a bundle is unknown");

  // The old whole-path substring match got this wrong: an ancestor folder name
  // must not decide the format of a file that only happens to live under it.
  ExpectFormat(PluginFormatFromPath("/plugins/lv2-backup/Foo.dll"), PluginFormat::VST2,
               "an ancestor folder name does not decide the format");

  Expect(PluginFormatId(PluginFormat::VST3) == "vst3", "VST3 keeps its stored id");
  Expect(PluginFormatId(PluginFormat::AudioUnit) == "au", "AudioUnit keeps its stored id");
  Expect(PluginFormatId(PluginFormat::Unknown).empty(), "Unknown has no stored id");
}

void TestBrowseFiltersCoverEveryBundle()
{
  const std::string filters = PluginBrowseFilters();
  // Echoed because the payload half is platform-specific: on a failure elsewhere
  // this is the first thing you want to see.
  std::cout << "  filters: " << filters << "\n";
  for (const char* suffix : {"*.vst3", "*.component", "*.appex", "*.lv2", "*.vst"})
    Expect(filters.find(suffix) != std::string::npos, std::string("filters offer ") + suffix);

  // Bundles are not always selectable, so the payload route must be offered too.
  Expect(filters.find("*.ttl") != std::string::npos, "filters offer the LV2 manifest payload");

  // CLAP and AAX cannot be hosted at all, so they are not dangled in the picker —
  // but they are still classified, so such a path arriving from a preset or an
  // existing library entry still gets a specific message.
  Expect(filters.find("*.clap") == std::string::npos, "filters do not offer .clap");
  Expect(filters.find("*.aaxplugin") == std::string::npos, "filters do not offer .aaxplugin");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.clap"), PluginFormat::CLAP,
               "a .clap path is still classified even though it is not offered");
  ExpectFormat(PluginFormatFromPath("/plugins/Foo.aaxplugin"), PluginFormat::AAX,
               "an .aaxplugin path is still classified even though it is not offered");
}
} // namespace

int main()
{
  const auto runTest = [](void (*test)(), const char* label) {
    const int before = gFailures;
    test();
    std::cout << label << (gFailures == before ? " PASS" : " FAIL") << std::endl;
  };

  runTest(TestBundleSuffixDetection, "PluginPathUtils detects bundle suffixes:");
  runTest(TestResolvesPayloadToBundleRoot, "PluginPathUtils resolves payloads to the bundle root:");
  runTest(TestSameNameAncestorWins, "PluginPathUtils climbs to a same-named bundle ancestor:");
  runTest(TestContainerFolderDoesNotSwallowPlugin, "PluginPathUtils stops at the nearest bundle:");
  runTest(TestNonBundlePathsArePreserved, "PluginPathUtils preserves non-bundle paths:");
  runTest(TestFormatDetection, "PluginPathUtils classifies plugin formats:");
  runTest(TestBrowseFiltersCoverEveryBundle, "PluginPathUtils derives browse filters:");

  return gFailures == 0 ? 0 : 1;
}
