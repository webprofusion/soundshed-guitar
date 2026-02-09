/*
 * Actually use plugin.hh / plugin.hxx to create a plugin. Assert that it is constructable
 */

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"

#include <type_traits>

#include <catch2/catch_all.hpp>

struct test_plugin : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                           clap::helpers::CheckingLevel::Maximal>
{
   const clap_plugin_descriptor *getDescription()
   {
      static const char *features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                       CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
      static clap_plugin_descriptor desc = {CLAP_VERSION,
                                            "org.free-audio.test-case.plugin",
                                            "Test Case Plugin",
                                            "Free Audio",
                                            "http://cleveraudio.org",
                                            "",
                                            "",
                                            "1.0.0",
                                            "This is a test only plugin",
                                            &features[0]};
      return &desc;
   }

   test_plugin(const clap_host *host) : clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                                              clap::helpers::CheckingLevel::Maximal>(getDescription(), host) {}
};

CATCH_TEST_CASE("Create an Actual Plugin")
{
   CATCH_SECTION("Test Plugin is Creatable")
   {
      CATCH_REQUIRE(std::is_constructible<test_plugin, const clap_host *>::value);
   }
}