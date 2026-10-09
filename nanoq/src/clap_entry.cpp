/*
 * clap_entry.cpp - the CLAP entry for Soundshed Nano Q.
 *
 * QPlug's own entry shim (q_plug_entry.cpp) hands the host's factory straight through. This one
 * does the same, with one addition: it notes which clap_host a plugin is being created for, so
 * the controller can ask that host to re-read parameter values after a state load. QPlug gives
 * a plugin no way to say that itself, and a CLAP host (clap-validator's state tests, and the
 * VST3 and AU wrappers built on CLAP) treats parameter values that change on load, without that
 * request, as a bug. The JUCE products make the same request through updateHostDisplay.
 *
 * Like QPlug's shim, this file is compiled once per plugin format by clap-wrapper, so each
 * binary exports the entry point under the name its format requires.
 */

#include <clap/clap.h>

#include <cstring>

extern "C" {
bool q_plug_entry_init(const char* plugin_path);
void q_plug_entry_deinit(void);
const void* q_plug_entry_get_factory(const char* factory_id);

// Defined by Nano Q's controller: the host the plugin about to be created belongs to.
void nanoq_set_creating_host(const clap_host_t* host);
}

namespace
{
const clap_plugin_factory_t* QFactory()
{
    return static_cast<const clap_plugin_factory_t*>(q_plug_entry_get_factory(CLAP_PLUGIN_FACTORY_ID));
}

uint32_t PluginCount(const clap_plugin_factory_t*)
{
    const auto* q = QFactory();
    return q != nullptr ? q->get_plugin_count(q) : 0;
}

const clap_plugin_descriptor_t* PluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    const auto* q = QFactory();
    return q != nullptr ? q->get_plugin_descriptor(q, index) : nullptr;
}

const clap_plugin_t* CreatePlugin(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    const auto* q = QFactory();
    if (q == nullptr)
        return nullptr;

    // QPlug builds the controller inside create_plugin, on this thread.
    nanoq_set_creating_host(host);
    const auto* plugin = q->create_plugin(q, host, id);
    nanoq_set_creating_host(nullptr);
    return plugin;
}

const clap_plugin_factory_t gFactory = {PluginCount, PluginDescriptor, CreatePlugin};

const void* GetFactory(const char* factoryId)
{
    if (factoryId != nullptr && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &gFactory;
    return q_plug_entry_get_factory(factoryId);
}
} // namespace

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION,
    q_plug_entry_init,
    q_plug_entry_deinit,
    GetFactory,
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
