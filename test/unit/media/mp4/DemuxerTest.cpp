// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/media/mp4/Demuxer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace ao::media::mp4::test
{
  TEST_CASE("MP4 Demuxer Resilience", "[media][unit][mp4][error]")
  {
    SECTION("Empty data returns FormatRejected")
    {
      auto const emptyData = std::vector<std::byte>{};
      auto demuxer = Demuxer{emptyData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Small garbage data returns FormatRejected")
    {
      auto const garbage = std::array{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
      auto demuxer = Demuxer{garbage};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Atom with missing stbl returns FormatRejected gracefully")
    {
      // Construct a very basic 'ftyp' + 'moov' structure but missing 'stbl'
      // ftyp atom (8 bytes header + payload)
      // moov atom (8 bytes header + no children)
      auto const data = std::vector<std::byte>{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10}, // size 16
        std::byte{'f'},  std::byte{'t'},  std::byte{'y'},  std::byte{'p'},  // type
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, // dummy
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, // dummy
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x08}, // size 8
        std::byte{'m'},  std::byte{'o'},  std::byte{'o'},  std::byte{'v'}   // type
      };

      auto demuxer = Demuxer{data};
      auto const result = demuxer.parseTrack("alac");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }
  }
} // namespace ao::media::mp4::test
