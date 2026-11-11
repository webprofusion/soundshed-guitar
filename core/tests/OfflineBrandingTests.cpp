/**
 * @file OfflineBrandingTests.cpp
 * @brief Holds this fork to its own name and to staying offline.
 *
 * The upstream project (Soundshed Guitar) does not license its name or its online services to
 * forks. So:
 *   - the product, company, domain and plugin ids are Sepia's own
 *   - settings and presets live in a folder of Sepia's own, never the upstream app's
 *   - online services are off, and no source file names an upstream host
 *   - the bundled factory pack is Sepia's, with none of the upstream pack's captures
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "GuitarFXConfig.h"
#include "util/FileSystem.h"

namespace
{
namespace fs = std::filesystem;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what << (detail.empty() ? "" : "  (" + detail + ")")
              << std::endl;
}

fs::path RepoRoot()
{
    return fs::weakly_canonical(fs::path(GUITARFX_DEMO_AUDIO_DIR) / ".." / ".." / "..");
}

std::string ReadText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

void TestBranding()
{
    std::cout << "\nName and ids" << std::endl;
    using namespace guitarfx::branding;
    Check(std::string(kProduct) == "Sepia", "the product is Sepia", kProduct);
    Check(std::string(kCompany) == "Sepia Audio Project", "the company is its own", kCompany);
    Check(std::string(kDomain) == "org.sepia.guitar", "the domain id is its own", kDomain);
    Check(std::string(kDisplay).find("Soundshed") == std::string::npos, "the display name does not borrow upstream's",
          kDisplay);
    Check(PLUG_UNIQUE_ID != 'SGFX' && PLUG_MFR_ID != 'SndS', "the plugin and manufacturer ids are not upstream's");
    Check(Lower(PLUG_URL_STR).find("soundshed.com") == std::string::npos &&
              std::string(PLUG_EMAIL_STR).find("soundshed") == std::string::npos,
          "it does not send users to upstream's site or support address");
    Check(!kOnlineServicesEnabled, "online services are off");
}

void TestDataFolder()
{
    std::cout << "\nData folder" << std::endl;
    const fs::path home = fs::temp_directory_path() / "sepia-branding-test-home";
#ifdef _WIN32
    _putenv_s("APPDATA", home.string().c_str());
#else
    setenv("HOME", home.string().c_str(), 1);
#endif
    const guitarfx::FileSystem fileSystem;
    const auto root = fileSystem.ResolvePlatformRootDirectory();
    Check(root.filename() == "Sepia", "settings live in a folder named Sepia", root.string());
    Check(root.string().find("Soundshed") == std::string::npos, "and nowhere under the upstream app's folder");
}

void TestNoUpstreamHost()
{
    std::cout << "\nNo upstream host in the source" << std::endl;
    const fs::path root = RepoRoot();
    // The scan covers the code that ships. Tests are left out: several name an upstream URL
    // precisely to check that it is refused.
    const std::vector<fs::path> trees = {root / "core" / "src", root / "core" / "config", root / "core" / "ui" / "ts",
                                         root / "core" / "ui" / "ui-components", root / "juce" / "source"};
    const std::vector<std::string> hosts = {"api-guitar.soundshed.com", "guitar.soundshed.com", "//soundshed.com"};
    int scanned = 0;
    std::vector<std::string> offenders;

    for (const auto& tree : trees)
    {
        if (!fs::exists(tree))
        {
            continue;
        }

        for (const auto& entry : fs::recursive_directory_iterator(tree))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const auto extension = entry.path().extension().string();

            if (extension != ".cpp" && extension != ".h" && extension != ".mm" && extension != ".ts" &&
                extension != ".html")
            {
                continue;
            }

            ++scanned;
            const std::string text = Lower(ReadText(entry.path()));

            for (const auto& host : hosts)
            {
                if (text.find(host) != std::string::npos)
                {
                    offenders.push_back(fs::relative(entry.path(), root).generic_string() + " names " + host);
                }
            }
        }
    }

    Check(scanned > 500, "the scan found the source tree", std::to_string(scanned) + " files");
    Check(offenders.empty(), "no shipped source file names an upstream host",
          offenders.empty() ? "" : offenders.front() + (offenders.size() > 1 ? " and others" : ""));
}

void TestFactoryPackIsOurs()
{
    std::cout << "\nBundled presets" << std::endl;
    const fs::path factory = RepoRoot() / "core" / "ui" / "presets" / "factory";
    int archives = 0;
    bool onlyOurs = true;

    if (fs::exists(factory))
    {
        for (const auto& entry : fs::directory_iterator(factory))
        {
            ++archives;
            onlyOurs = onlyOurs && entry.path().filename().string().rfind("Sepia-", 0) == 0;
        }
    }

    Check(archives == 1 && onlyOurs, "the only bundled pack is Sepia's own", std::to_string(archives) + " file(s)");
    // The upstream pack's captures were megabytes; a pack of built-in effects is a few kilobytes.
    Check(!fs::exists(factory / "Sepia-Factory.soundshed.presets") ||
              fs::file_size(factory / "Sepia-Factory.soundshed.presets") < 200 * 1024,
          "and it carries no capture or impulse response files");
    Check(std::string(GUITARFX_DEFAULT_STARTUP_PRESET_TITLE) == "Warm Clean Room",
          "the app opens on a preset from that pack", GUITARFX_DEFAULT_STARTUP_PRESET_TITLE);
}
} // namespace

int main()
{
    std::cout << "=== OfflineBrandingTests ===" << std::endl;
    TestBranding();
    TestDataFolder();
    TestNoUpstreamHost();
    TestFactoryPackIsOurs();
    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
