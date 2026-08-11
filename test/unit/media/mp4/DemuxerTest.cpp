// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/Demuxer.h>

#include "TestAtoms.h"
#include <ao/utility/MappedFile.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <type_traits>
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

  static_assert(!std::is_constructible_v<Demuxer, std::span<std::byte const>>);
  static_assert(std::is_nothrow_move_constructible_v<Demuxer>);
  static_assert(!std::is_move_assignable_v<Demuxer>);

  TEST_CASE("MP4 Demuxer - rejects malformed sample-table inputs", "[media][unit][mp4][error]")
  {
    SECTION("Empty data returns FormatRejected")
    {
      auto const emptyData = std::vector<std::byte>{};
      auto const result = Demuxer::parse(emptyData, "alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Small garbage data returns FormatRejected")
    {
      auto const garbage = std::array{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
      auto const result = Demuxer::parse(garbage, "alac");
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

      auto const result = Demuxer::parse(data, "alac");

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Short audio sample entry returns FormatRejected")
    {
      auto const shortEntry = ao::test::mp4::makeAtom("alac", {});
      auto const stsd = ao::test::mp4::makeStsdAtomFromSampleEntry(shortEntry);
      auto const stbl = ao::test::mp4::makeSampleTableAtom(stsd);
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto const result = Demuxer::parse(fileData, "alac");
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
      auto const result = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
    }

    SECTION("Unsupported sample description mappings are rejected")
    {
      auto const stsc = ao::test::mp4::makeStscAtom(1, 2);
      auto const stbl =
        makeSampleTable(makeAlacStsd(), ao::test::mp4::makeStszAtom(4), stsc, ao::test::mp4::makeStcoAtom());
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto const result = Demuxer::parse(fileData, "alac");
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
      auto const result = Demuxer::parse(fileData, "alac");
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
      auto const result = Demuxer::parse(fileData, "alac");
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
      auto const result = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::FormatRejected);
      CHECK(result.error().message == "MP4 sample offset overflow");
    }
  }

  TEST_CASE("MP4 Demuxer - selected track ignores unrelated trailing structure", "[media][regression][mp4]")
  {
    auto const config = ao::test::mp4::makeAtom("alac", {9, 8, 7});
    auto const track = ao::test::mp4::makeCompleteAudioTrackAtom("alac", config, 48000, 96000, 7, 2048, 321);

    SECTION("Extended-size mdat after moov")
    {
      auto data = ao::test::mp4::makeAtom("moov", track);
      auto const mdat = ao::test::mp4::makeExtendedAtom("mdat", {1, 2, 3});
      data.insert(data.end(), mdat.begin(), mdat.end());
      auto const fileData = toBytes(data);
      REQUIRE(Demuxer::parse(fileData, "alac"));
    }

    SECTION("End-of-file mdat after moov")
    {
      auto data = ao::test::mp4::makeAtom("moov", track);
      auto const mdat = ao::test::mp4::makeEndOfFileAtom("mdat", {1, 2, 3});
      data.insert(data.end(), mdat.begin(), mdat.end());
      auto const fileData = toBytes(data);
      REQUIRE(Demuxer::parse(fileData, "alac"));
    }

    SECTION("Malformed sibling after selected track")
    {
      auto moovBody = track;
      auto const malformedSibling = std::array<std::uint8_t, 8>{0x00, 0x00, 0x00, 0x10, 'f', 'r', 'e', 'e'};
      moovBody.insert(moovBody.end(), malformedSibling.begin(), malformedSibling.end());
      auto const fileData = toBytes(ao::test::mp4::makeAtom("moov", moovBody));
      REQUIRE(Demuxer::parse(fileData, "alac"));
    }
  }

  TEST_CASE("MP4 Demuxer - parses version 1 media timing", "[media][unit][mp4]")
  {
    auto const stbl = ao::test::mp4::makeSampleTableAtom(makeAlacStsd());
    auto const mdhd = ao::test::mp4::makeMdhdVersion1Atom(48000, 96000);
    auto const track = ao::test::mp4::makeTrackAtomWithMdhd("soun", stbl, mdhd);
    auto const fileData = makeFile(track);
    auto const demuxerRes = Demuxer::parse(fileData, "alac");

    REQUIRE(demuxerRes);
    CHECK(demuxerRes->timescale() == 48000);
    CHECK(demuxerRes->duration() == 96000);
  }

  TEST_CASE("MP4 Demuxer - parses extended-size media timing and sample tables", "[media][regression][mp4]")
  {
    auto const stsd = ao::test::mp4::makeExtendedFromCompactAtom(makeAlacStsd());
    auto const stsz = ao::test::mp4::makeExtendedFromCompactAtom(ao::test::mp4::makeStszAtom(7));
    auto const stsc = ao::test::mp4::makeExtendedFromCompactAtom(ao::test::mp4::makeStscAtom());
    auto const stco = ao::test::mp4::makeExtendedFromCompactAtom(ao::test::mp4::makeStcoAtom(321));
    auto const stbl = makeSampleTable(stsd, stsz, stsc, stco);
    auto const mdhd = ao::test::mp4::makeExtendedFromCompactAtom(ao::test::mp4::makeMdhdAtom(48000, 96000));
    auto const track = ao::test::mp4::makeTrackAtomWithMdhd("soun", stbl, mdhd);
    auto const fileData = makeFile(track);
    auto const demuxerRes = Demuxer::parse(fileData, "alac");

    REQUIRE(demuxerRes);
    CHECK(demuxerRes->timescale() == 48000);
    CHECK(demuxerRes->duration() == 96000);
    CHECK(demuxerRes->sampleCount() == 1);
    CHECK(demuxerRes->sampleInfo(0).offset == 321);
    CHECK(demuxerRes->sampleInfo(0).size == 7);
  }

  TEST_CASE("MP4 Demuxer - extracts AAC AudioSpecificConfig", "[media][unit][mp4]")
  {
    auto const testFile = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto mappedFile = utility::MappedFile{};
    REQUIRE(mappedFile.map(testFile));

    auto const result = Demuxer::parse(mappedFile.bytes(), "mp4a");

    REQUIRE(result);
    CHECK_FALSE(result->magicCookie().empty());
    CHECK(result->sampleCount() > 0);
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
    auto const result = Demuxer::parse(fileData, "mp4a");

    REQUIRE(result);
    CHECK(result->timescale() == 48000);
    CHECK(result->duration() == 96000);
    CHECK(result->sampleCount() == 1);
    CHECK(result->sampleInfo(0).offset == 321);
    CHECK(result->sampleInfo(0).size == 7);
    CHECK(result->sampleInfo(0).duration == 2048);
    REQUIRE(result->magicCookie().size() == 2);
    CHECK(result->magicCookie()[0] == std::byte{0x12});
    CHECK(result->magicCookie()[1] == std::byte{0x10});
  }
} // namespace ao::media::mp4::test
