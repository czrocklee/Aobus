// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TestAtoms.h"
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/MappedFile.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
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

    std::vector<std::uint8_t> makeAlacStsd()
    {
      auto const config = ao::test::mp4::makeAtom("alac", {9, 8, 7});
      return ao::test::mp4::makeStsdAtom("alac", config);
    }

    std::vector<std::uint8_t> makeSampleTable(std::vector<std::uint8_t> const& stsd,
                                              std::vector<std::uint8_t> const& stsz,
                                              std::vector<std::uint8_t> const& stsc,
                                              std::vector<std::uint8_t> const& chunkOffsets)
    {
      auto body = std::vector<std::uint8_t>{};
      auto const stts = ao::test::mp4::makeSttsAtom();
      body.insert(body.end(), stsd.begin(), stsd.end());
      body.insert(body.end(), stsz.begin(), stsz.end());
      body.insert(body.end(), stts.begin(), stts.end());
      body.insert(body.end(), stsc.begin(), stsc.end());
      body.insert(body.end(), chunkOffsets.begin(), chunkOffsets.end());
      return ao::test::mp4::makeAtom("stbl", body);
    }

    std::vector<std::uint8_t> makeSampleTable(std::vector<std::uint8_t> const& stsd,
                                              std::vector<std::uint8_t> const& stsz,
                                              std::vector<std::uint8_t> const& chunkOffsets)
    {
      return makeSampleTable(stsd, stsz, ao::test::mp4::makeStscAtom(), chunkOffsets);
    }

    std::vector<std::byte> makeFile(std::vector<std::uint8_t> const& track)
    {
      return toBytes(ao::test::mp4::makeAtom("moov", track));
    }
  } // namespace

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
      auto const data = std::vector{
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

    SECTION("Short audio sample entry returns FormatRejected")
    {
      auto const shortEntry = ao::test::mp4::makeAtom("alac", {});
      auto const stsd = ao::test::mp4::makeStsdAtomFromSampleEntry(shortEntry);
      auto const stbl = ao::test::mp4::makeSampleTableAtom(stsd);
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto demuxer = Demuxer{fileData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Multiple sample descriptions are not selected implicitly")
    {
      auto const alacConfig = ao::test::mp4::makeAtom("alac", {9, 8, 7});
      auto const alacEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", alacConfig);
      auto const mp4aEntry = ao::test::mp4::makeAudioSampleEntryAtom("mp4a", ao::test::mp4::makeAtom("esds", {}));
      auto const stsd = ao::test::mp4::makeStsdAtomFromSampleEntries({alacEntry, mp4aEntry});
      auto const stbl = ao::test::mp4::makeSampleTableAtom(stsd);
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto demuxer = Demuxer{fileData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Unsupported sample description mappings are rejected")
    {
      auto const stsc = ao::test::mp4::makeStscAtom(1, 2);
      auto const stbl =
        makeSampleTable(makeAlacStsd(), ao::test::mp4::makeStszAtom(4), stsc, ao::test::mp4::makeStcoAtom());
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto demuxer = Demuxer{fileData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "Invalid MP4 sample-to-chunk entry");
    }

    SECTION("First sample-to-chunk entry must start at the first chunk")
    {
      auto const stsc = ao::test::mp4::makeStscAtom(1, 1, 2);
      auto const stbl =
        makeSampleTable(makeAlacStsd(), ao::test::mp4::makeStszAtom(4), stsc, ao::test::mp4::makeStcoAtom());
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto demuxer = Demuxer{fileData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "MP4 sample-to-chunk entry references an invalid chunk");
    }

    SECTION("Declared sample count must fit the stsz atom")
    {
      auto stszBody = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(stszBody, 0);
      ao::test::mp4::appendBe32(stszBody, 0);
      ao::test::mp4::appendBe32(stszBody, std::numeric_limits<std::uint32_t>::max());
      auto const stsz = ao::test::mp4::makeAtom("stsz", stszBody);
      auto const stbl = makeSampleTable(makeAlacStsd(), stsz, ao::test::mp4::makeStcoAtom());
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto demuxer = Demuxer{fileData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "Malformed stsz entry table");
    }

    SECTION("64-bit chunk offset arithmetic cannot wrap")
    {
      auto co64Body = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(co64Body, 0);
      ao::test::mp4::appendBe32(co64Body, 1);
      ao::test::mp4::appendBe64(co64Body, std::numeric_limits<std::uint64_t>::max() - 1U);
      auto const co64 = ao::test::mp4::makeAtom("co64", co64Body);
      auto const stbl = makeSampleTable(makeAlacStsd(), ao::test::mp4::makeStszAtom(4), co64);
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto demuxer = Demuxer{fileData};

      auto const result = demuxer.parseTrack("alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "MP4 sample offset overflow");
    }
  }

  TEST_CASE("MP4 Demuxer - parses version 1 media timing", "[media][unit][mp4]")
  {
    auto const stbl = ao::test::mp4::makeSampleTableAtom(makeAlacStsd());
    auto const mdhd = ao::test::mp4::makeMdhdVersion1Atom(48000, 96000);
    auto const track = ao::test::mp4::makeTrackAtomWithMdhd("soun", stbl, mdhd);
    auto const fileData = makeFile(track);
    auto demuxer = Demuxer{fileData};

    REQUIRE(demuxer.parseTrack("alac"));
    CHECK(demuxer.timescale() == 48000);
    CHECK(demuxer.duration() == 96000);
  }

  TEST_CASE("MP4 Demuxer - extracts AAC AudioSpecificConfig", "[media][unit][mp4]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto mappedFile = utility::MappedFile{};
    REQUIRE(mappedFile.map(testFile));

    auto demuxer = Demuxer{mappedFile.bytes()};
    auto const result = demuxer.parseTrack("mp4a");

    REQUIRE(result);
    CHECK_FALSE(demuxer.magicCookie().empty());
    CHECK(demuxer.sampleCount() > 0);
  }

  TEST_CASE("MP4 Demuxer - binds sample table to selected audio track", "[media][unit][mp4]")
  {
    auto const esdsAtom = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0, 0x05, 0x02, 0x12, 0x10});
    auto const audioTrack = ao::test::mp4::makeCompleteAudioTrackAtom("mp4a", esdsAtom, 48000, 96000, 7, 2048, 321);
    auto const videoTrack = ao::test::mp4::makeVideoTrackAtom("avc1");

    auto moovBody = std::vector<std::uint8_t>{};
    moovBody.insert(moovBody.end(), videoTrack.begin(), videoTrack.end());
    moovBody.insert(moovBody.end(), audioTrack.begin(), audioTrack.end());

    auto data = std::vector<std::uint8_t>{};
    ao::test::mp4::addAtom(data, "moov", moovBody);

    auto fileData = toBytes(data);
    auto demuxer = Demuxer{fileData};
    auto const result = demuxer.parseTrack("mp4a");

    REQUIRE(result);
    CHECK(demuxer.timescale() == 48000);
    CHECK(demuxer.duration() == 96000);
    CHECK(demuxer.sampleCount() == 1);
    CHECK(demuxer.sampleInfo(0).offset == 321);
    CHECK(demuxer.sampleInfo(0).size == 7);
    CHECK(demuxer.sampleInfo(0).duration == 2048);
    REQUIRE(demuxer.magicCookie().size() == 2);
    CHECK(demuxer.magicCookie()[0] == std::byte{0x12});
    CHECK(demuxer.magicCookie()[1] == std::byte{0x10});
  }
} // namespace ao::media::mp4::test
