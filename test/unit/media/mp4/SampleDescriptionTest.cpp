// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/media/mp4/SampleDescription.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::media::mp4::test
{
  namespace
  {
    std::vector<std::byte> toBytes(std::vector<std::uint8_t> const& bytes)
    {
      auto result = std::vector<std::byte>{};
      result.reserve(bytes.size());

      for (auto const byte : bytes)
      {
        result.push_back(static_cast<std::byte>(byte));
      }

      return result;
    }
  } // namespace

  TEST_CASE("MP4 SampleDescription - reads audio sample entry type", "[media][unit][mp4]")
  {
    SECTION("Returns ALAC sample entry type")
    {
      auto const fileData = toBytes(ao::test::mp4::makeMinimalAudioMp4("alac"));

      CHECK(audioSampleEntryType(fileData) == "alac");
    }

    SECTION("Returns AAC sample entry type")
    {
      auto const esdsAtom = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0});
      auto const fileData = toBytes(ao::test::mp4::makeMinimalAudioMp4("mp4a", esdsAtom));

      CHECK(audioSampleEntryType(fileData) == "mp4a");
    }

    SECTION("Returns unknown sample entry types for dispatch decisions")
    {
      auto const fileData = toBytes(ao::test::mp4::makeMinimalAudioMp4("zzzz"));

      CHECK(audioSampleEntryType(fileData) == "zzzz");
    }

    SECTION("Returns empty string when stsd is missing or malformed")
    {
      auto const missingStsd = toBytes(ao::test::mp4::makeAtom("moov", {}));
      auto const malformed = std::vector{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};

      CHECK(audioSampleEntryType(missingStsd).empty());
      CHECK(audioSampleEntryType(malformed).empty());
    }
  }
} // namespace ao::media::mp4::test
