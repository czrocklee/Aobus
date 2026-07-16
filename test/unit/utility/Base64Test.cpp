// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Base64.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace ao::utility::test
{
  TEST_CASE("Base64 - encodes decodes and rejects malformed inputs", "[utility][unit][base64]")
  {
    SECTION("Empty input")
    {
      auto const data = std::vector<std::byte>{};
      auto const encoded = base64Encode(data);
      CHECK(encoded.empty());

      // Decoding a valid (empty) input yields an engaged optional holding an empty vector,
      // distinct from the nullopt returned on malformed input.
      auto const optDecoded = base64Decode(encoded);
      REQUIRE(optDecoded);
      CHECK(optDecoded->empty());
    }

    SECTION("Small string round-trip")
    {
      auto const input = std::string_view{"Hello Aobus!"};
      auto const data = std::span{reinterpret_cast<std::byte const*>(input.data()), input.size()};

      auto const encoded = base64Encode(data);
      CHECK(encoded == "SGVsbG8gQW9idXMh");

      auto const optDecoded = base64Decode(encoded);
      REQUIRE(optDecoded);
      auto const result = std::string_view{reinterpret_cast<char const*>(optDecoded->data()), optDecoded->size()};
      CHECK(result == input);
    }

    SECTION("Binary data round-trip (non-multiple of 3)")
    {
      auto const data =
        std::vector{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}, std::byte{0x42}};

      auto const encoded = base64Encode(data);
      auto const optDecoded = base64Decode(encoded);
      CHECK(optDecoded == data);
    } // std::optional<vector> == vector compares the engaged value

    SECTION("Large binary data round-trip")
    {
      auto data = std::vector<std::byte>(1024);

      for (std::size_t i = 0; i < 1024; ++i)
      {
        data[i] = static_cast<std::byte>(i & 0xFF);
      }

      auto const encoded = base64Encode(data);
      auto const optDecoded = base64Decode(encoded);
      REQUIRE(optDecoded);
      CHECK(optDecoded->size() == data.size());
      CHECK(optDecoded == data);
    }

    SECTION("Invalid characters in decode")
    {
      auto const optInvalid = base64Decode("SGVsbG8h@#$");
      CHECK_FALSE(optInvalid);
    }

    SECTION("Ignore whitespace in decode")
    {
      auto const* const input = "SGVsbG8g\nQW9idXMh"; // Hello Aobus!
      auto const optDecoded = base64Decode(input);
      REQUIRE(optDecoded);
      auto const result = std::string_view{reinterpret_cast<char const*>(optDecoded->data()), optDecoded->size()};
      CHECK(result == "Hello Aobus!");
    }

    SECTION("Padding handling")
    {
      // 1 byte -> 2 chars + 2 padding
      auto const d1 = std::vector{std::byte{'A'}};
      CHECK(base64Encode(d1) == "QQ==");
      CHECK(base64Decode("QQ==") == d1);

      // 2 bytes -> 3 chars + 1 padding
      auto const d2 = std::vector{std::byte{'A'}, std::byte{'B'}};
      CHECK(base64Encode(d2) == "QUI=");
      CHECK(base64Decode("QUI=") == d2);
    }

    SECTION("Truncation and invalid padding bits")
    {
      // Invalid length (1 mod 4)
      CHECK_FALSE(base64Decode("Q").has_value());

      // Valid length but non-zero padding bits
      // 'A' encodes to "QQ==", where the second 'Q' uses 2 bits and 4 bits are zero padding.
      // If we change the second char to 'R' (which has non-zero in the bottom 4 bits):
      CHECK_FALSE(base64Decode("QR==").has_value());

      // Unpadded base64 should still work if valid
      auto const d1 = std::vector{std::byte{'A'}};
      CHECK(base64Decode("QQ") == d1);
    }

    SECTION("Padding terminates the encoded value exactly")
    {
      CHECK_FALSE(base64Decode("QQ==garbage"));
      CHECK_FALSE(base64Decode("QQ==="));
      CHECK_FALSE(base64Decode("QQ="));
      CHECK_FALSE(base64Decode("QUJD="));
      CHECK(base64Decode("QQ== \n\t") == std::vector{std::byte{'A'}});
    }
  }
} // namespace ao::utility::test
