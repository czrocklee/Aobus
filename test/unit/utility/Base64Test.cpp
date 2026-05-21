// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/utility/Base64.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace ao::utility::test
{
  TEST_CASE("Base64 Utility - Encoding and Decoding", "[utility][base64]")
  {
    SECTION("Empty input")
    {
      auto const data = std::vector<std::byte>{};
      auto const encoded = base64Encode(data);
      CHECK(encoded.empty());

      auto const decoded = base64Decode(encoded);
      CHECK(decoded.empty());
    }

    SECTION("Small string round-trip")
    {
      auto const input = std::string_view{"Hello Aobus!"};
      auto const data = std::span{reinterpret_cast<std::byte const*>(input.data()), input.size()};

      auto const encoded = base64Encode(data);
      CHECK(encoded == "SGVsbG8gQW9idXMh");

      auto const decoded = base64Decode(encoded);
      auto const result = std::string_view{reinterpret_cast<char const*>(decoded.data()), decoded.size()};
      CHECK(result == input);
    }

    SECTION("Binary data round-trip (non-multiple of 3)")
    {
      auto const data =
        std::vector<std::byte>{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}, std::byte{0x42}};

      auto const encoded = base64Encode(data);
      auto const decoded = base64Decode(encoded);
      CHECK(decoded == data);
    }

    SECTION("Large binary data round-trip")
    {
      auto data = std::vector<std::byte>(1024);
      
for (std::size_t i = 0; i < 1024; ++i)
      {
        data[i] = static_cast<std::byte>(i & 0xFF);
      }

      auto const encoded = base64Encode(data);
      auto const decoded = base64Decode(encoded);
      REQUIRE(decoded.size() == data.size());
      CHECK(decoded == data);
    }

    SECTION("Invalid characters in decode")
    {
      auto const invalid = base64Decode("SGVsbG8h@#$");
      CHECK(invalid.empty());
    }

    SECTION("Ignore whitespace in decode")
    {
      const auto *const input = "SGVsbG8g\nQW9idXMh"; // Hello Aobus!
      auto const decoded = base64Decode(input);
      auto const result = std::string_view{reinterpret_cast<char const*>(decoded.data()), decoded.size()};
      CHECK(result == "Hello Aobus!");
    }

    SECTION("Padding handling")
    {
      // 1 byte -> 2 chars + 2 padding
      auto const d1 = std::vector<std::byte>{std::byte{'A'}};
      CHECK(base64Encode(d1) == "QQ==");
      CHECK(base64Decode("QQ==") == d1);

      // 2 bytes -> 3 chars + 1 padding
      auto const d2 = std::vector<std::byte>{std::byte{'A'}, std::byte{'B'}};
      CHECK(base64Encode(d2) == "QUI=");
      CHECK(base64Decode("QUI=") == d2);
    }

    SECTION("Truncation and invalid padding bits")
    {
      // Invalid length (1 mod 4)
      CHECK(base64Decode("Q").empty());

      // Valid length but non-zero padding bits
      // 'A' encodes to "QQ==", where the second 'Q' uses 2 bits and 4 bits are zero padding.
      // If we change the second char to 'R' (which has non-zero in the bottom 4 bits):
      CHECK(base64Decode("QR==").empty());

      // Unpadded base64 should still work if valid
      auto const d1 = std::vector<std::byte>{std::byte{'A'}};
      CHECK(base64Decode("QQ") == d1);
    }
  }
} // namespace ao::utility::test
