#include <clap/helpers/preset-discovery-provider.hxx>

#include <catch2/catch_all.hpp>

template class clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Terminate,
                                                      clap::helpers::CheckingLevel::Maximal>;
template class clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Terminate,
                                                      clap::helpers::CheckingLevel::Minimal>;
template class clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Terminate,
                                                      clap::helpers::CheckingLevel::None>;
template class clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Ignore,
                                                      clap::helpers::CheckingLevel::Maximal>;

CATCH_TEST_CASE("PresetDiscoveryProvider - Link") {
   clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Terminate,
                                          clap::helpers::CheckingLevel::Maximal> *p0 = nullptr;
   clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Terminate,
                                          clap::helpers::CheckingLevel::Minimal> *p1 = nullptr;
   clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Terminate,
                                          clap::helpers::CheckingLevel::None> *p2 = nullptr;
   clap::helpers::PresetDiscoveryProvider<clap::helpers::MisbehaviourHandler::Ignore,
                                          clap::helpers::CheckingLevel::Maximal> *p3 = nullptr;
}