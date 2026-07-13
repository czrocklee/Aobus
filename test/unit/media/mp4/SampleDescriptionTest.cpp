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

    std::vector<std::byte> makeFileWithStsd(std::vector<std::uint8_t> const& stsdAtom)
    {
      auto const stblAtom = ao::test::mp4::makeAtom("stbl", stsdAtom);
      auto const minfAtom = ao::test::mp4::makeAtom("minf", stblAtom);
      auto const hdlrAtom = ao::test::mp4::makeHdlrAtom("soun");
      auto mdiaBody = std::vector<std::uint8_t>{};
      mdiaBody.insert(mdiaBody.end(), hdlrAtom.begin(), hdlrAtom.end());
      mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());
      auto const mdiaAtom = ao::test::mp4::makeAtom("mdia", mdiaBody);
      auto const trakAtom = ao::test::mp4::makeAtom("trak", mdiaAtom);
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "moov", trakAtom);
      return toBytes(data);
    }
  } // namespace

  TEST_CASE("MP4 SampleDescription - reads audio sample entry type", "[media][unit][mp4]")
  {
    SECTION("Returns ALAC sample entry type")
    {
      auto const fileData = toBytes(ao::test::mp4::makeMinimalAudioMp4("alac"));
      auto const result = audioSampleEntryType(fileData);

      REQUIRE(result);
      CHECK(*result == "alac");
    }

    SECTION("Returns AAC sample entry type")
    {
      auto const esdsAtom = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0});
      auto const fileData = toBytes(ao::test::mp4::makeMinimalAudioMp4("mp4a", esdsAtom));
      auto const result = audioSampleEntryType(fileData);

      REQUIRE(result);
      CHECK(*result == "mp4a");
    }

    SECTION("Reads an extended-size sample description")
    {
      auto const stsdAtom = ao::test::mp4::makeExtendedFromCompactAtom(ao::test::mp4::makeStsdAtom("alac"));
      auto const result = audioSampleEntryType(makeFileWithStsd(stsdAtom));

      REQUIRE(result);
      CHECK(*result == "alac");
    }

    SECTION("Skips non-audio tracks before the audio sample entry")
    {
      auto data = std::vector<std::uint8_t>{};
      auto moovBody = std::vector<std::uint8_t>{};
      auto const videoTrack = ao::test::mp4::makeVideoTrackAtom("avc1");
      auto const audioTrack = ao::test::mp4::makeAudioTrackAtom("mp4a", ao::test::mp4::makeAtom("esds", {0, 0, 0, 0}));
      moovBody.insert(moovBody.end(), videoTrack.begin(), videoTrack.end());
      moovBody.insert(moovBody.end(), audioTrack.begin(), audioTrack.end());
      ao::test::mp4::addAtom(data, "moov", moovBody);
      auto const result = audioSampleEntryType(toBytes(data));

      REQUIRE(result);
      CHECK(*result == "mp4a");
    }

    SECTION("Accepts supported audio sample entries when hdlr is absent")
    {
      auto const stsdAtom = ao::test::mp4::makeStsdAtom("alac");
      auto const stblAtom = ao::test::mp4::makeAtom("stbl", stsdAtom);
      auto const minfAtom = ao::test::mp4::makeAtom("minf", stblAtom);
      auto const mdiaAtom = ao::test::mp4::makeAtom("mdia", minfAtom);
      auto const trakAtom = ao::test::mp4::makeAtom("trak", mdiaAtom);
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "moov", trakAtom);
      auto const result = audioSampleEntryType(toBytes(data));

      REQUIRE(result);
      CHECK(*result == "alac");
    }

    SECTION("Accepts supported audio sample entries when hdlr is too short")
    {
      auto const stsdAtom = ao::test::mp4::makeStsdAtom("alac");
      auto const stblAtom = ao::test::mp4::makeAtom("stbl", stsdAtom);
      auto const minfAtom = ao::test::mp4::makeAtom("minf", stblAtom);
      auto const hdlrAtom = ao::test::mp4::makeAtom("hdlr", {});
      auto mdiaBody = std::vector<std::uint8_t>{};
      mdiaBody.insert(mdiaBody.end(), hdlrAtom.begin(), hdlrAtom.end());
      mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());
      auto const mdiaAtom = ao::test::mp4::makeAtom("mdia", mdiaBody);
      auto const trakAtom = ao::test::mp4::makeAtom("trak", mdiaAtom);
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "moov", trakAtom);
      auto const result = audioSampleEntryType(toBytes(data));

      REQUIRE(result);
      CHECK(*result == "alac");
    }

    SECTION("Rejects unsupported sample entries when hdlr is absent")
    {
      auto const stsdAtom = ao::test::mp4::makeStsdAtom("zzzz");
      auto const stblAtom = ao::test::mp4::makeAtom("stbl", stsdAtom);
      auto const minfAtom = ao::test::mp4::makeAtom("minf", stblAtom);
      auto const mdiaAtom = ao::test::mp4::makeAtom("mdia", minfAtom);
      auto const trakAtom = ao::test::mp4::makeAtom("trak", mdiaAtom);
      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "moov", trakAtom);
      auto const result = audioSampleEntryType(toBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("Returns unknown sample entry types for dispatch decisions")
    {
      auto const fileData = toBytes(ao::test::mp4::makeMinimalAudioMp4("zzzz"));
      auto const result = audioSampleEntryType(fileData);

      REQUIRE(result);
      CHECK(*result == "zzzz");
    }

    SECTION("Reports NotFound when stsd is missing")
    {
      auto const missingStsd = toBytes(ao::test::mp4::makeAtom("moov", {}));
      auto const result = audioSampleEntryType(missingStsd);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("Preserves malformed container errors")
    {
      auto const malformed = std::vector{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
      auto const result = audioSampleEntryType(malformed);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Reports NotFound for malformed stsd sample entries")
    {
      auto data = std::vector<std::uint8_t>{};
      auto moovBody = std::vector<std::uint8_t>{};
      auto stsdBody = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1};
      auto const stsdAtom = ao::test::mp4::makeAtom("stsd", stsdBody);
      auto const stblAtom = ao::test::mp4::makeAtom("stbl", stsdAtom);
      auto const minfAtom = ao::test::mp4::makeAtom("minf", stblAtom);
      auto const hdlrAtom = ao::test::mp4::makeHdlrAtom("soun");
      auto mdiaBody = std::vector<std::uint8_t>{};
      mdiaBody.insert(mdiaBody.end(), hdlrAtom.begin(), hdlrAtom.end());
      mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());
      auto const mdiaAtom = ao::test::mp4::makeAtom("mdia", mdiaBody);
      auto const trakAtom = ao::test::mp4::makeAtom("trak", mdiaAtom);
      moovBody.insert(moovBody.end(), trakAtom.begin(), trakAtom.end());
      ao::test::mp4::addAtom(data, "moov", moovBody);
      auto const result = audioSampleEntryType(toBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("Reports NotFound when stsd has no entries")
    {
      auto const stsdAtom = ao::test::mp4::makeAtom("stsd", {0, 0, 0, 0, 0, 0, 0, 0});
      auto const result = audioSampleEntryType(makeFileWithStsd(stsdAtom));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("Reports NotFound when stsd has multiple entries")
    {
      auto const alacEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac");
      auto const mp4aEntry = ao::test::mp4::makeAudioSampleEntryAtom("mp4a", ao::test::mp4::makeAtom("esds", {}));
      auto const stsdAtom = ao::test::mp4::makeStsdAtomFromSampleEntries({alacEntry, mp4aEntry});
      auto const result = audioSampleEntryType(makeFileWithStsd(stsdAtom));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
    }

    SECTION("Preserves a malformed sample entry boundary error")
    {
      auto stsdBody = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1};
      stsdBody.insert(stsdBody.end(), {0, 0, 0, 100, 'a', 'l', 'a', 'c'});
      auto const stsdAtom = ao::test::mp4::makeAtom("stsd", stsdBody);
      auto const result = audioSampleEntryType(makeFileWithStsd(stsdAtom));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::media::mp4::test
