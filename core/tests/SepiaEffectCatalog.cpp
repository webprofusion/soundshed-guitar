/**
 * @file SepiaEffectCatalog.cpp
 * @brief Prints every registered effect as JSON: ids, aliases, parameters and factory presets.
 *
 * tools/sepia-presets/build_pack.py builds the factory preset archive from this, so the pack is
 * always written against the parameters this build really has. Run with a path to write the
 * catalog there; with none it only checks that the catalog is complete, which is what ctest runs.
 */

#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"

int main(int argc, char** argv)
{
    guitarfx::RegisterAllEffects();

    nlohmann::json effects = nlohmann::json::array();
    int failures = 0;

    for (const auto& info : guitarfx::EffectRegistry::Instance().GetAllTypes())
    {
        nlohmann::json parameters = nlohmann::json::array();

        for (const auto& param : info.parameters)
        {
            parameters.push_back({{"id", param.id},
                                  {"name", param.displayName},
                                  {"default", param.defaultValue},
                                  {"min", param.minValue},
                                  {"max", param.maxValue},
                                  {"unit", param.unit},
                                  {"labels", param.labels}});
        }

        nlohmann::json presets = nlohmann::json::array();

        for (const auto& preset : info.presets)
        {
            presets.push_back({{"id", preset.id}, {"name", preset.displayName}, {"parameters", preset.parameters}});
        }

        if (info.aliases.empty())
        {
            std::cout << "  [FAIL] " << info.displayName << " has no alias to write presets against" << std::endl;
            ++failures;
        }

        effects.push_back({{"type", info.type},
                           {"aliases", info.aliases},
                           {"displayName", info.displayName},
                           {"category", info.category},
                           {"requiresResource", info.requiresResource},
                           {"parameters", parameters},
                           {"presets", presets}});
    }

    if (effects.empty())
    {
        std::cout << "  [FAIL] no effects are registered" << std::endl;
        ++failures;
    }

    if (argc > 1)
    {
        std::ofstream out(argv[1], std::ios::binary);
        out << nlohmann::json{{"effects", effects}}.dump(1) << std::endl;

        if (!out)
        {
            std::cout << "  [FAIL] could not write " << argv[1] << std::endl;
            ++failures;
        }
    }

    std::cout << effects.size() << " effects in the catalog" << std::endl;
    return failures == 0 ? 0 : 1;
}
