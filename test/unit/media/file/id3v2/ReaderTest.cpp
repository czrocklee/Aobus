// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/media/file/mpeg/id3v2/Frame.h"
#include "lib/media/file/mpeg/id3v2/Layout.h"
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace ao::media::file::mpeg::id3v2::test
{
  TEST_CASE("ID3v2 - converts text encodings to UTF-8", "[media][unit][mpeg][id3v2]")
  {
    SECTION("Latin1 to UTF-8")
    {
      auto input = std::string{"H\xE9llo"}; // Latin1 'é' is 0xE9
      auto output = convertToUtf8(utility::bytes::view(input), Encoding::Latin1);
      CHECK(output == "H\xC3\xA9llo"); // UTF-8 'é' is 0xC3 0xA9
    }

    SECTION("UCS-2 BE to UTF-8")
    {
      // BOM (FE FF) + "Test" in UTF-16BE
      auto input = std::vector{std::byte{0xFE},
                               std::byte{0xFF},
                               std::byte{0x00},
                               std::byte{'T'},
                               std::byte{0x00},
                               std::byte{'e'},
                               std::byte{0x00},
                               std::byte{'s'},
                               std::byte{0x00},
                               std::byte{'t'}};
      auto output = convertToUtf8(input, Encoding::Ucs2);
      CHECK(output == "Test");
    }

    SECTION("UCS-2 LE to UTF-8")
    {
      // BOM (FF FE) + "Test" in UTF-16LE
      auto input = std::vector{std::byte{0xFF},
                               std::byte{0xFE},
                               std::byte{'T'},
                               std::byte{0x00},
                               std::byte{'e'},
                               std::byte{0x00},
                               std::byte{'s'},
                               std::byte{0x00},
                               std::byte{'t'},
                               std::byte{0x00}};
      auto output = convertToUtf8(input, Encoding::Ucs2);
      CHECK(output == "Test");
    }

    SECTION("UCS-2 without BOM (defaults to BE)")
    {
      auto input = std::vector{std::byte{0x00}, std::byte{'T'}, std::byte{0x00}, std::byte{'e'}};
      auto output = convertToUtf8(input, Encoding::Ucs2);
      CHECK(output == "Te");
    }

    SECTION("UTF-8 passes through unchanged (ID3v2.4)")
    {
      auto input = std::string{"H\xC3\xA9llo"}; // already UTF-8 'é'
      auto output = convertToUtf8(utility::bytes::view(input), Encoding::Utf8);
      CHECK(output == "H\xC3\xA9llo");
    }

    SECTION("UTF-8 strips a leading BOM (ID3v2.4)")
    {
      auto input = std::vector{std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}, std::byte{'H'}, std::byte{'i'}};
      auto output = convertToUtf8(input, Encoding::Utf8);
      CHECK(output == "Hi");
    }

    SECTION("UTF-16BE without BOM (ID3v2.4)")
    {
      auto input = std::vector{std::byte{0x00},
                               std::byte{'T'},
                               std::byte{0x00},
                               std::byte{'e'},
                               std::byte{0x00},
                               std::byte{'s'},
                               std::byte{0x00},
                               std::byte{'t'}};
      auto output = convertToUtf8(input, Encoding::Utf16Be);
      CHECK(output == "Test");
    }

    SECTION("UTF-16BE decodes surrogate pairs (ID3v2.4)")
    {
      auto input = std::vector{std::byte{0xD8}, std::byte{0x3D}, std::byte{0xDE}, std::byte{0x00}};
      auto output = convertToUtf8(input, Encoding::Utf16Be);
      CHECK(output == std::string{"\xF0\x9F\x98\x80"});
    }
  }
} // namespace ao::media::file::mpeg::id3v2::test
