#include <clap/helpers/hex-encoder.hh>

#include <catch2/catch_all.hpp>

namespace {

   CATCH_TEST_CASE("hex encoder") {
      const std::vector<uint8_t> data{0xF0, 0xA8, 0xC4, 0x0F, 0xFF};
      const std::string encoded = clap::helpers::hex_encode(data.data(), data.size());
      CATCH_CHECK(encoded == "f0a8c40fff");

      const std::vector<uint8_t> decoded = clap::helpers::hex_decode(encoded);
      CATCH_CHECK(data == decoded);
   }

   CATCH_TEST_CASE("hex decoder - upper case hex") {
      const std::vector<uint8_t> data{0xF0, 0xA8, 0xC4, 0x0F, 0xFF};
      const std::vector<uint8_t> decoded = clap::helpers::hex_decode("F0A8C40FFF");
      CATCH_CHECK(data == decoded);
   }

   CATCH_TEST_CASE("hex decoder - bad char") {
      CATCH_CHECK_THROWS(clap::helpers::hex_decode("yl"));
   }

   CATCH_TEST_CASE("hex decoder - bad length") {
      CATCH_CHECK_THROWS(clap::helpers::hex_decode("f3d"));
   }
} // namespace