/**
 * EffectLayoutPreferences.cpp - Which layout an effect is drawn with: the preference rules in
 * the app settings, resolved as the web UI's layoutPreferences.ts resolves them (see
 * EffectLayouts.h).
 */

#include "uiclient/EffectLayouts.h"

#include "uiclient/NodeLabels.h"
#include "util/Base64.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <set>

namespace guitarfx::uiclient
{
namespace
{
std::string StringOf(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return {};
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string Lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{}
                                      : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace

// ── Choosing a layout ───────────────────────────────────────────────────────────────────

std::vector<LayoutPreferenceRule> ParseLayoutPreferenceRules(const nlohmann::json& appSettings)
{
    std::vector<LayoutPreferenceRule> rules;

    if (!appSettings.is_object())
    {
        return rules;
    }

    const auto it = appSettings.find(kLayoutPreferencesSetting);

    if (it == appSettings.end() || !it->is_array())
    {
        return rules;
    }

    for (const auto& raw : *it)
    {
        LayoutPreferenceRule rule;
        rule.lookupKey = Trim(StringOf(raw, "lookupKey"));
        rule.layoutId = Trim(StringOf(raw, "layoutId"));
        rule.scope = StringOf(raw, "scope");

        if (rule.lookupKey.empty() || rule.layoutId.empty() ||
            (rule.scope != "effectType" && rule.scope != "keyword" && rule.scope != "preset"))
        {
            continue;
        }

        const auto keyword = Lower(Trim(StringOf(raw, "keyword")));
        const auto presetId = Trim(StringOf(raw, "presetId"));

        if ((rule.scope == "keyword" && keyword.empty()) || (rule.scope == "preset" && presetId.empty()))
        {
            continue;
        }

        rule.keyword = rule.scope == "keyword" ? keyword : std::string{};
        rule.presetId = rule.scope == "preset" ? presetId : std::string{};
        rule.id = StringOf(raw, "id");
        rule.presetName = StringOf(raw, "presetName");
        rule.createdAt = StringOf(raw, "createdAt");
        rules.push_back(std::move(rule));
    }

    return rules;
}

std::vector<std::string> LayoutLookupKeys(const std::string& effectType, const std::string& blendId)
{
    std::vector<std::string> keys{blendId.empty() ? effectType : effectType + "::" + blendId};

    if (keys.front() != effectType)
    {
        keys.push_back(effectType);
    }

    return keys;
}

LayoutSelection ResolveLayoutSelection(const LayoutLibrary* library, const std::vector<LayoutPreferenceRule>& rules,
                                       const LayoutResolutionContext& context)
{
    const auto keys = LayoutLookupKeys(context.effectType, context.blendId);
    const auto matchText = Lower(context.matchText);

    for (const auto& key : keys)
    {
        std::vector<const LayoutPreferenceRule*> keyed;

        for (const auto& rule : rules)
        {
            if (rule.lookupKey == key)
            {
                keyed.push_back(&rule);
            }
        }

        if (keyed.empty())
        {
            continue;
        }

        if (!context.presetId.empty())
        {
            for (const auto* rule : keyed)
            {
                if (rule->scope == "preset" && rule->presetId == context.presetId)
                {
                    return {rule->layoutId, "preset", *rule};
                }
            }
        }

        // The longest keyword wins, so "vox ac30" beats a broader "vox".
        const LayoutPreferenceRule* best = nullptr;

        for (const auto* rule : keyed)
        {
            if (rule->scope == "keyword" && !matchText.empty() && matchText.find(rule->keyword) != std::string::npos &&
                (best == nullptr || rule->keyword.size() > best->keyword.size()))
            {
                best = rule;
            }
        }

        if (best != nullptr)
        {
            return {best->layoutId, "keyword", *best};
        }

        for (const auto* rule : keyed)
        {
            if (rule->scope == "effectType")
            {
                return {rule->layoutId, "effectType", *rule};
            }
        }
    }

    if (library != nullptr)
    {
        for (const auto& key : keys)
        {
            const auto def = library->defaults.find(key);
            const auto entries = library->byEffectType.find(key);

            if (def == library->defaults.end() || def->second.empty() || entries == library->byEffectType.end())
            {
                continue;
            }

            const bool exists =
                std::any_of(entries->second.begin(), entries->second.end(),
                            [&def](const LayoutLibraryEntry& entry) { return entry.layoutId == def->second; });

            if (exists)
            {
                return {def->second, "libraryDefault", std::nullopt};
            }
        }
    }

    return {std::nullopt, "none", std::nullopt};
}

std::vector<const LayoutLibraryEntry*> AvailableLayoutEntries(const LayoutLibrary& library,
                                                              const std::string& effectType, const std::string& blendId)
{
    std::vector<const LayoutLibraryEntry*> entries;
    std::set<std::string> seen;

    for (const auto& key : LayoutLookupKeys(effectType, blendId))
    {
        const auto it = library.byEffectType.find(key);

        if (it == library.byEffectType.end())
        {
            continue;
        }

        for (const auto& entry : it->second)
        {
            if (!entry.layoutId.empty() && seen.insert(entry.layoutId).second)
            {
                entries.push_back(&entry);
            }
        }
    }

    return entries;
}

const EffectLayout* DefaultLibraryLayout(const LayoutLibrary& library, const std::string& effectType,
                                         const std::string& blendId)
{
    const auto find = [&library](const std::string& key) -> const EffectLayout* {
        const auto id = library.defaults.find(key);
        const auto entries = library.byEffectType.find(key);

        if (id == library.defaults.end() || id->second.empty() || entries == library.byEffectType.end())
        {
            return nullptr;
        }

        for (const auto& entry : entries->second)
        {
            if (entry.layoutId == id->second)
            {
                return &entry.layout;
            }
        }

        return nullptr;
    };

    if (!blendId.empty())
    {
        if (const auto* layout = find(effectType + "::" + blendId))
        {
            return layout;
        }
    }

    return find(effectType);
}

std::string ImageDataUrlBytes(const std::string& dataUrl)
{
    static const char* const kTypes[] = {"png", "jpeg", "gif", "webp"};
    std::size_t start = std::string::npos;

    for (const char* type : kTypes)
    {
        const std::string prefix = std::string("data:image/") + type + ";base64,";

        if (dataUrl.rfind(prefix, 0) == 0)
        {
            start = prefix.size();
            break;
        }
    }

    if (start == std::string::npos)
    {
        return {};
    }

    for (std::size_t i = start; i < dataUrl.size(); ++i)
    {
        const char c = dataUrl[i];

        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '/' && c != '=')
        {
            return {};
        }
    }

    const auto bytes = util::DecodeBase64(dataUrl.substr(start));
    return std::string(bytes.begin(), bytes.end());
}

const EffectLayout* FindLayoutById(const LayoutLibrary& library, const std::string& layoutId,
                                   const std::string& effectType, const std::string& blendId)
{
    if (layoutId.empty() || layoutId == kStandardLayoutId)
    {
        return nullptr;
    }

    for (const auto* entry : AvailableLayoutEntries(library, effectType, blendId))
    {
        if (entry->layoutId == layoutId)
        {
            return &entry->layout;
        }
    }

    return nullptr;
}

std::string LayoutMatchText(const ClientState& state, const GraphNode& node)
{
    // The automatic name is what the node is called without the user's title.
    GraphNode untitled = node;
    untitled.title.clear();
    const auto* typeInfo = state.FindEffectType(node.type);

    std::string resources;

    for (std::size_t i = 0; i < node.resources.size(); ++i)
    {
        const auto name = NodeResourceName(state, node, i);

        if (!name.empty())
        {
            resources += resources.empty() ? name : " + " + name;
        }
    }

    std::string text;

    for (const auto& part : {NodeDisplayName(state, untitled), node.title, resources,
                             typeInfo != nullptr ? typeInfo->name : std::string{}})
    {
        if (!Trim(part).empty())
        {
            text += text.empty() ? part : " " + part;
        }
    }

    return Lower(text);
}

LayoutResolutionContext LayoutContextForNode(const ClientState& state, const GraphNode& node)
{
    LayoutResolutionContext context;
    context.effectType = node.type;

    if (const auto it = node.config.find("blendId"); it != node.config.end())
    {
        context.blendId = it->second;
    }

    context.matchText = LayoutMatchText(state, node);
    context.presetId = state.activePresetId;
    return context;
}

const EffectLayout* ResolveLayoutForNode(const ClientState& state, const GraphNode& node)
{
    if (!state.layouts.loaded)
    {
        return nullptr;
    }

    const auto context = LayoutContextForNode(state, node);
    const auto selection =
        ResolveLayoutSelection(&state.layouts.library, ParseLayoutPreferenceRules(state.appSettings), context);

    if (!selection.layoutId || *selection.layoutId == kStandardLayoutId)
    {
        return nullptr;
    }

    return FindLayoutById(state.layouts.library, *selection.layoutId, context.effectType, context.blendId);
}

namespace
{
std::string NewRuleId()
{
    // Ids as the web UI makes them: the time and a few random characters, in base 36.
    const auto base36 = [](std::uint64_t value) {
        static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string text;

        do
        {
            text.insert(text.begin(), kDigits[value % 36]);
            value /= 36;
        } while (value > 0);

        return text;
    };
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::random_device random;
    return "layoutpref-" + base36(static_cast<std::uint64_t>(millis)) + "-" + base36(random() % 2176782336u);
}

nlohmann::json SerializeRules(const std::vector<LayoutPreferenceRule>& rules, const std::string& nowIso)
{
    nlohmann::json serialized = nlohmann::json::array();

    for (const auto& r : rules)
    {
        nlohmann::json entry{{"id", r.id}, {"lookupKey", r.lookupKey}, {"scope", r.scope}, {"layoutId", r.layoutId}};

        if (!r.keyword.empty())
        {
            entry["keyword"] = r.keyword;
        }

        if (!r.presetId.empty())
        {
            entry["presetId"] = r.presetId;
        }

        if (!r.presetName.empty())
        {
            entry["presetName"] = r.presetName;
        }

        entry["createdAt"] = r.createdAt.empty() ? nowIso : r.createdAt;
        serialized.push_back(std::move(entry));
    }

    return serialized;
}

std::string TrimLower(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        return {};
    }

    auto out = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
} // namespace

nlohmann::json RulesWithLayoutChoice(const nlohmann::json& appSettings, const ClientState& state, const GraphNode& node,
                                     const std::string& layoutId, const std::string& nowIso)
{
    const auto context = LayoutContextForNode(state, node);
    const auto rules = ParseLayoutPreferenceRules(appSettings);
    const auto deciding = ResolveLayoutSelection(&state.layouts.library, rules, context).rule;

    LayoutPreferenceRule rule;
    // A blend's choice is keyed on the blend, as the web UI's picker keys it.
    rule.lookupKey = LayoutLookupKeys(context.effectType, context.blendId).front();
    rule.scope = deciding ? deciding->scope : std::string("effectType");
    rule.layoutId = layoutId;
    rule.keyword = deciding ? deciding->keyword : std::string{};
    rule.presetId = deciding ? deciding->presetId : std::string{};
    rule.presetName = deciding ? deciding->presetName : std::string{};
    return RulesWithLayoutPreference(appSettings, std::move(rule), nowIso);
}

nlohmann::json RulesWithLayoutPreference(const nlohmann::json& appSettings, LayoutPreferenceRule rule,
                                         const std::string& nowIso)
{
    auto rules = ParseLayoutPreferenceRules(appSettings);
    rule.keyword = rule.scope == "keyword" ? TrimLower(rule.keyword) : std::string{};

    if (rule.scope != "preset")
    {
        rule.presetId.clear();
        rule.presetName.clear();
    }

    rule.id = NewRuleId();
    rule.createdAt = nowIso;
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&rule](const LayoutPreferenceRule& existing) {
                                   return existing.lookupKey == rule.lookupKey && existing.scope == rule.scope &&
                                          existing.keyword == rule.keyword && existing.presetId == rule.presetId;
                               }),
                rules.end());
    rules.push_back(std::move(rule));
    return SerializeRules(rules, nowIso);
}

nlohmann::json RulesWithoutLayoutPreference(const nlohmann::json& appSettings, const std::string& ruleId)
{
    auto rules = ParseLayoutPreferenceRules(appSettings);
    std::erase_if(rules, [&ruleId](const LayoutPreferenceRule& r) { return r.id == ruleId; });
    return SerializeRules(rules, {});
}

std::vector<LayoutPreferenceRule> LayoutRulesForKeys(const nlohmann::json& appSettings,
                                                     const std::vector<std::string>& lookupKeys)
{
    auto rules = ParseLayoutPreferenceRules(appSettings);
    std::erase_if(rules, [&lookupKeys](const LayoutPreferenceRule& r) {
        return std::find(lookupKeys.begin(), lookupKeys.end(), r.lookupKey) == lookupKeys.end();
    });
    return rules;
}

std::vector<std::string> SuggestLayoutKeywords(const std::string& matchText, std::size_t limit)
{
    static const std::set<std::string> kStopWords{"the",   "and",  "amp",     "amplifier", "model", "nam",
                                                  "ir",    "cab",  "cabinet", "wav",       "mix",   "channel",
                                                  "clean", "lead", "rhythm",  "high",      "low",   "gain"};
    std::vector<std::string> out;
    std::string token;
    const auto flush = [&] {
        const bool number = !token.empty() && std::all_of(token.begin(), token.end(),
                                                          [](unsigned char c) { return std::isdigit(c) != 0; });

        if (token.size() >= 3 && !number && kStopWords.count(token) == 0 &&
            std::find(out.begin(), out.end(), token) == out.end() && out.size() < limit)
        {
            out.push_back(token);
        }

        token.clear();
    };

    for (const char raw : matchText)
    {
        const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+')
        {
            token.push_back(c);
        }
        else
        {
            flush();
        }
    }

    flush();
    return out;
}

std::string LayoutRuleScopeLabel(const LayoutPreferenceRule& rule)
{
    if (rule.scope == "preset")
    {
        return "Preset: " + (!rule.presetName.empty() ? rule.presetName
                             : !rule.presetId.empty() ? rule.presetId
                                                      : std::string("(unknown)"));
    }

    if (rule.scope == "keyword")
    {
        return "Matching \"" + rule.keyword + "\"";
    }

    return "This effect type";
}

std::string LayoutSourceNote(const LayoutSelection& selection)
{
    if (selection.source == "preset")
    {
        return "Currently set by a preset rule.";
    }

    if (selection.source == "keyword")
    {
        return "Currently set by the \"" + (selection.rule ? selection.rule->keyword : std::string()) +
               "\" keyword rule.";
    }

    if (selection.source == "effectType")
    {
        return "Currently set for this effect type.";
    }

    if (selection.source == "libraryDefault")
    {
        return "Currently using the layout library default.";
    }

    return "No layout preference set, showing standard controls.";
}
} // namespace guitarfx::uiclient
