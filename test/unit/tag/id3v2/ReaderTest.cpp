// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/mpeg/id3v2/Frame.h"
#include "lib/tag/mpeg/id3v2/Layout.h"

#include <bits/basic_string.h>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace std::string_literals;

namespace ao::tag::mpeg::id3v2::test
{
  TEST_CASE("ID3v2 - UTF-8 Conversion", "[tag][mpeg][id3v2]")
  {
    SECTION("Latin1 to UTF-8")
    {
      auto input = "H\xE9llo"s; // Latin1 'é' is 0xE9
      auto output = convertToUtf8(input.data(), input.data() + input.size(), Encoding::Latin1);
      CHECK(output == "H\xC3\xA9llo"); // UTF-8 'é' is 0xC3 0xA9
    }

    SECTION("UCS-2 BE to UTF-8")
    {
      // BOM (FE FF) + "Test" in UTF-16BE
      auto input = std::vector<char>{char(0xFE), char(0xFF), 0x00, 'T', 0x00, 'e', 0x00, 's', 0x00, 't'};
      auto output = convertToUtf8(input.data(), input.data() + input.size(), Encoding::Ucs2);
      CHECK(output == "Test");
    }

    SECTION("UCS-2 LE to UTF-8")
    {
      // BOM (FF FE) + "Test" in UTF-16LE
      auto input = std::vector<char>{char(0xFF), char(0xFE), 'T', 0x00, 'e', 0x00, 's', 0x00, 't', 0x00};
      auto output = convertToUtf8(input.data(), input.data() + input.size(), Encoding::Ucs2);
      CHECK(output == "Test");
    }

    SECTION("UCS-2 without BOM (defaults to BE)")
    {
      auto input = std::vector<char>{0x00, 'T', 0x00, 'e'};
      auto output = convertToUtf8(input.data(), input.data() + input.size(), Encoding::Ucs2);
      CHECK(output == "Te");
    }
  }
} // namespace ao::tag::mpeg::id3v2::test
