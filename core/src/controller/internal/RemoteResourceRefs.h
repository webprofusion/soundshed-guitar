#pragma once

/**
 * RemoteResourceRefs.h - Models and IRs a preset archive names rather than carries.
 *
 * A file whose licence, or whose provider's terms, do not allow passing it on is shared as a
 * reference: {id, type, name, provider, providerKey, hash?, contentHash?, providerData?, licence?, ...}, the
 * shape the web UI writes (core/ui/ts/remoteResources.ts). Whoever installs the archive must
 * already have the file, or fetch it from its provider. Tone3000 was the first such provider and
 * has its own older list, `tone3000Resources`, which is read here too.
 */

#include <nlohmann/json.hpp>

#include <string>

namespace guitarfx
{
class ResourceLibrary;
}

namespace guitarfx::controller_detail
{
/// The archive's remote references from `remoteResources` and the older `tone3000Resources`
/// (each as provider "tone3000", its toneId and modelId in providerData), one per library id.
[[nodiscard]] nlohmann::json ReadArchiveRemoteResources(const nlohmann::json& root);

/// The library id of the resource a reference names, or empty. Tried in turn: the same library
/// id; the same provider and provider key (a Tone3000 model also by its tone and model ids, as
/// imports recorded it before provider keys); the same file hash (the engine's FNV of every
/// byte); the same model content hash (models/ContentHash.h), which also finds a renamed or
/// re-tagged copy that came from somewhere else.
[[nodiscard]] std::string FindRemoteResource(const ResourceLibrary& library, const nlohmann::json& reference);
} // namespace guitarfx::controller_detail
