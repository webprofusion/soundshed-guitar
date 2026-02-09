#include <clap/helpers/context-menu-builder.hh>

#include <type_traits>
#include <catch2/catch_all.hpp>

namespace {

   struct TestBuilder : clap::helpers::ContextMenuBuilder {
      TestBuilder() {}
      bool addItem(clap_context_menu_item_kind_t item_kind, const void *item_data) override
      {
         return false;
      }
      bool supports(clap_context_menu_item_kind_t item_kind) const noexcept override
      {
         return false;
      }
   };

   CATCH_TEST_CASE("Context Menu Builder exists") {
      CATCH_REQUIRE(std::is_constructible<TestBuilder>::value);
   }

} // namespace
