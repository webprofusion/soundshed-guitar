#include <clap/helpers/plugin-proxy.hxx>
#include <clap/helpers/host.hh>
#include <clap/helpers/host.hxx>

#include <catch2/catch_all.hpp>

template class clap::helpers::Host<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Maximal>;
template class clap::helpers::Host<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>;
template class clap::helpers::Host<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::None>;
template class clap::helpers::Host<clap::helpers::MisbehaviourHandler::Ignore,
                                   clap::helpers::CheckingLevel::Maximal>;

template class clap::helpers::PluginProxy<clap::helpers::MisbehaviourHandler::Terminate,
                                          clap::helpers::CheckingLevel::Maximal>;
template class clap::helpers::PluginProxy<clap::helpers::MisbehaviourHandler::Terminate,
                                          clap::helpers::CheckingLevel::Minimal>;
template class clap::helpers::PluginProxy<clap::helpers::MisbehaviourHandler::Terminate,
                                          clap::helpers::CheckingLevel::None>;
template class clap::helpers::PluginProxy<clap::helpers::MisbehaviourHandler::Ignore,
                                          clap::helpers::CheckingLevel::Maximal>;

CATCH_TEST_CASE("Host - Link") {
   clap::helpers::Host<clap::helpers::MisbehaviourHandler::Terminate,
                       clap::helpers::CheckingLevel::Maximal> *h0 = nullptr;
   clap::helpers::Host<clap::helpers::MisbehaviourHandler::Terminate,
                       clap::helpers::CheckingLevel::Minimal> *h1 = nullptr;
   clap::helpers::Host<clap::helpers::MisbehaviourHandler::Terminate,
                       clap::helpers::CheckingLevel::None> *h2 = nullptr;
   clap::helpers::Host<clap::helpers::MisbehaviourHandler::Ignore,
                       clap::helpers::CheckingLevel::Maximal> *h3 = nullptr;
}
