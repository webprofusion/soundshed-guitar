#include "PresetServiceClient.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib.h"
#include "presets/PresetStorage.h"

namespace namguitar
{
namespace
{
struct UrlComponents
{
	std::string scheme;
	std::string host;
	int port = 80;
	std::string path;
};

UrlComponents ParseBaseUrl(const std::string& url)
{
	UrlComponents components;
	if (url.empty())
	{
		return components;
	}

	std::string trimmed = url;
	const auto first = trimmed.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
	{
		return components;
	}
	const auto last = trimmed.find_last_not_of(" \t\r\n");
	trimmed = trimmed.substr(first, last - first + 1);

	std::size_t authorityStart = 0;
	const auto schemePos = trimmed.find("//");
	if (schemePos != std::string::npos && schemePos >= 1 && trimmed[schemePos - 1] == ':')
	{
		components.scheme = trimmed.substr(0, schemePos - 1);
		std::transform(components.scheme.begin(), components.scheme.end(), components.scheme.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		authorityStart = schemePos + 2;
	}
	else
	{
		components.scheme = "http";
	}

	const auto pathPos = trimmed.find('/', authorityStart);
	const std::string authority = trimmed.substr(authorityStart, pathPos == std::string::npos ? std::string::npos : pathPos - authorityStart);

	const auto colonPos = authority.find(':');
	if (colonPos != std::string::npos)
	{
		components.host = authority.substr(0, colonPos);
		const std::string portString = authority.substr(colonPos + 1);
		if (!portString.empty())
		{
			try
			{
				components.port = std::stoi(portString);
			}
			catch (...)
			{
				components.port = components.scheme == "https" ? 443 : 80;
			}
		}
	}
	else
	{
		components.host = authority;
		components.port = components.scheme == "https" ? 443 : 80;
	}

	if (pathPos != std::string::npos)
	{
		components.path = trimmed.substr(pathPos);
	}

	if (components.path.empty())
	{
		components.path = "/";
	}

	return components;
}

std::string CombinePath(const std::string& base, const std::string& leaf)
{
	if (base.empty())
	{
		return leaf.empty() || leaf.front() == '/' ? leaf : '/' + leaf;
	}

	if (leaf.empty())
	{
		return base;
	}

	if (base.back() == '/' && leaf.front() == '/')
	{
		return base + leaf.substr(1);
	}

	if (base.back() != '/' && leaf.front() != '/')
	{
		return base + '/' + leaf;
	}

	return base + leaf;
}

std::unique_ptr<httplib::Client> CreateClient(const UrlComponents& components)
{
	if (components.host.empty())
	{
		return nullptr;
	}

	std::unique_ptr<httplib::Client> client;
	if (components.scheme == "https")
	{
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
		auto https = std::make_unique<httplib::SSLClient>(components.host, components.port);
		https->enable_server_certificate_verification(true);
		client = std::move(https);
#else
		return nullptr;
#endif
	}
	else
	{
		client = std::make_unique<httplib::Client>(components.host, components.port);
	}

	client->set_connection_timeout(5, 0);
	client->set_read_timeout(15, 0);
	client->set_write_timeout(15, 0);
	return client;
}

std::string BuildQuery(const PresetSearchRequest& request)
{
	std::string query;
	if (!request.query.empty())
	{
		query += "query=" + httplib::detail::encode_url(request.query);
	}
	if (!request.category.empty())
	{
		if (!query.empty())
		{
			query += '&';
		}
		query += "category=" + httplib::detail::encode_url(request.category);
	}
	return query;
}

Preset ParsePresetJson(const nlohmann::json& jsonPreset)
{
	Preset preset;
	preset.id = jsonPreset.value("id", "");
	preset.name = jsonPreset.value("name", "");
	preset.category = jsonPreset.value("category", "");
	preset.description = jsonPreset.value("description", "");
	preset.audioFxModelId = jsonPreset.value("audioFxModelId", "");
	preset.irId = jsonPreset.value("irId", "");
	preset.fxChain = jsonPreset.value("fxChain", std::vector<std::string>{});

	if (jsonPreset.contains("attachments") && jsonPreset["attachments"].is_array())
	{
		for (const auto& attachmentJson : jsonPreset["attachments"])
		{
			PresetAttachment attachment;
			attachment.type = attachmentJson.value("type", "");
			attachment.filePath = std::filesystem::path(attachmentJson.value("path", ""));
			attachment.hash = attachmentJson.value("hash", "");
			preset.attachments.push_back(std::move(attachment));
		}
	}

	if (jsonPreset.contains("parameters") && jsonPreset["parameters"].is_array())
	{
		for (const auto& parameterJson : jsonPreset["parameters"])
		{
			PresetParameter parameter;
			parameter.id = parameterJson.value("id", "");
			parameter.value = parameterJson.value("value", 0.0);
			preset.parameters.push_back(std::move(parameter));
		}
	}

	return preset;
}

std::vector<Preset> ParsePresetResponse(const nlohmann::json& json)
{
	std::vector<Preset> presets;

	const auto appendPreset = [&](const nlohmann::json& item) {
		presets.push_back(ParsePresetJson(item));
	};

	if (json.is_array())
	{
		for (const auto& item : json)
		{
			appendPreset(item);
		}
	}
	else if (json.contains("presets") && json["presets"].is_array())
	{
		for (const auto& item : json["presets"])
		{
			appendPreset(item);
		}
	}
	else if (!json.is_null())
	{
		appendPreset(json);
	}

	return presets;
}

void DispatchResult(PresetServiceClient::ResultCallback& callback, std::vector<Preset>&& presets)
{
	if (callback)
	{
		callback(std::move(presets));
	}
}

} // namespace

void PresetServiceClient::SetBaseUrl(std::string baseUrl)
{
	mBaseUrl = std::move(baseUrl);
}

void PresetServiceClient::SearchPresets(const PresetSearchRequest& request, ResultCallback callback)
{
	std::thread([this, request, callback = std::move(callback)]() mutable {
		std::vector<Preset> presets;

		if (!mBaseUrl.empty())
		{
			const auto components = ParseBaseUrl(mBaseUrl);
			if (auto client = CreateClient(components))
			{
				std::string path = CombinePath(components.path, "presets");
				const std::string query = BuildQuery(request);
				if (!query.empty())
				{
					path += '?' + query;
				}

				if (auto response = client->Get(path.c_str()); response && response->status == 200)
				{
					auto json = nlohmann::json::parse(response->body, nullptr, false);
					if (!json.is_discarded())
					{
						presets = ParsePresetResponse(json);
					}
				}
			}
		}

		DispatchResult(callback, std::move(presets));
	}).detach();
}

void PresetServiceClient::DownloadPreset(const std::string& presetId, ResultCallback callback)
{
	std::thread([this, presetId, callback = std::move(callback)]() mutable {
		std::vector<Preset> presets;

		if (!mBaseUrl.empty() && !presetId.empty())
		{
			const auto components = ParseBaseUrl(mBaseUrl);
			if (auto client = CreateClient(components))
			{
				const std::string path = CombinePath(components.path, "presets/" + httplib::detail::encode_url(presetId));

				if (auto response = client->Get(path.c_str()); response && response->status == 200)
				{
					auto json = nlohmann::json::parse(response->body, nullptr, false);
					if (!json.is_discarded())
					{
						presets = ParsePresetResponse(json);
					}
				}
			}
		}

		DispatchResult(callback, std::move(presets));
	}).detach();
}

} // namespace namguitar
