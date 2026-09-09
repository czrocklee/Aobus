// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/mp4/Demuxer.h>

#include "TestAtoms.h"
#include <ao/utility/MappedFile.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
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
                                              std::vector<std::uint8_t> const& chunkOffsets,
                                              std::vector<std::uint8_t> const& stts = ao::test::mp4::makeSttsAtom())
    {
      auto body = std::vector<std::uint8_t>{};
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

  TEST_CASE("MP4 Demuxer - rejects infeasible fixed-size sample counts before expansion", "[media][regression][mp4]")
  {
    constexpr auto kHugeCount = std::numeric_limits<std::uint32_t>::max();
    auto sampleCount = kHugeCount;
    std::uint32_t sampleSize = 1;
    auto stsc = ao::test::mp4::makeStscAtom(kHugeCount);
    auto stts = ao::test::mp4::makeSttsAtom(kHugeCount);
    auto chunkOffsets = ao::test::mp4::makeStcoAtom();

    SECTION("Matching compact tables cannot justify more fixed-size bytes than the file")
    {
    }

    SECTION("Missing chunk entries cannot admit the declared count")
    {
      stsc.clear();
    }

    SECTION("A small chunk mapping cannot admit the declared count")
    {
      stsc = ao::test::mp4::makeStscAtom();
    }

    SECTION("Missing timing keeps the byte-feasibility requirement")
    {
      stts.clear();
    }

    SECTION("Zero samples are rejected")
    {
      sampleCount = 0;
      stsc = ao::test::mp4::makeStscAtom();
      stts = ao::test::mp4::makeSttsAtom();
    }

    SECTION("Fixed-size byte multiplication cannot wrap at 32 bits")
    {
      sampleCount = 2;
      sampleSize = 0x80000000U;
      stsc = ao::test::mp4::makeStscAtom(2);
      stts = ao::test::mp4::makeSttsAtom(2);
    }

    SECTION("Chunk expansion cannot exceed the sample count")
    {
      sampleCount = 2;
      sampleSize = 4;
      stts = ao::test::mp4::makeSttsAtom(2);
    }

    SECTION("Timing expansion cannot exceed the sample count")
    {
      sampleCount = 2;
      sampleSize = 4;
      stsc = ao::test::mp4::makeStscAtom(2);
    }

    SECTION("Chunk-run multiplication cannot wrap at 32 bits")
    {
      auto body = std::vector<std::uint8_t>{};

      for (auto const value : {0U, 2U, 0U, 1U})
      {
        ao::test::mp4::appendBe32(body, value);
      }

      chunkOffsets = ao::test::mp4::makeAtom("stco", body);
    }

    SECTION("Timing entries must cover every sample")
    {
      sampleCount = 2;
      stsc = ao::test::mp4::makeStscAtom(2);
      stts = ao::test::mp4::makeSttsAtom();
    }

    SECTION("Timing deltas must be nonzero")
    {
      sampleCount = 1;
      stsc = ao::test::mp4::makeStscAtom();
      stts = ao::test::mp4::makeSttsAtom(1, 0);
    }

    auto const stsz = ao::test::mp4::makeStszAtom(sampleSize, sampleCount);
    auto const stbl = makeSampleTable(makeAlacStsd(), stsz, stsc, chunkOffsets, stts);
    auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
    REQUIRE(fileData.size() < 512);
    auto const res = Demuxer::parse(fileData, "alac");
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
  }

  TEST_CASE("MP4 Demuxer - fixed-size sample index must fit the mapped file budget", "[media][regression][mp4]")
  {
    constexpr std::uint32_t kSampleCount = 1024;
    auto const fileSize = GENERATE(std::size_t{kSampleCount},
                                   (kSampleCount * sizeof(Demuxer::SampleEntry)) - 1,
                                   kSampleCount * sizeof(Demuxer::SampleEntry));
    CAPTURE(fileSize);
    auto const stbl = makeSampleTable(makeAlacStsd(),
                                      ao::test::mp4::makeStszAtom(1, kSampleCount),
                                      ao::test::mp4::makeStscAtom(kSampleCount),
                                      ao::test::mp4::makeStcoAtom(),
                                      ao::test::mp4::makeSttsAtom(kSampleCount));
    auto fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
    REQUIRE(fileData.size() < fileSize);
    fileData.resize(fileSize);

    if (auto const res = Demuxer::parse(fileData, "alac"); fileSize < kSampleCount * sizeof(Demuxer::SampleEntry))
    {
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message == "MP4 fixed-size samples or index exceed file size");
    }
    else
    {
      REQUIRE(res);
      CHECK(res->sampleCount() == kSampleCount);
      CHECK(res->sampleInfo(kSampleCount - 1).offset == kSampleCount - 1);
      CHECK(res->samplePayload(kSampleCount - 1).size() == 1);
    }
  }

  TEST_CASE("MP4 Demuxer - rejects malformed compact sample table entries", "[media][regression][mp4]")
  {
    auto stsz = ao::test::mp4::makeStszAtom();
    auto stsc = ao::test::mp4::makeStscAtom();
    auto stts = ao::test::mp4::makeSttsAtom();
    auto expectedMessage = std::string_view{};

    SECTION("Chunk mappings must have strictly increasing first chunks")
    {
      auto body = std::vector<std::uint8_t>{};

      // Two mappings both begin at chunk one.
      for (auto const value : {0U, 2U, 1U, 1U, 1U, 1U, 1U, 1U})
      {
        ao::test::mp4::appendBe32(body, value);
      }

      stsc = ao::test::mp4::makeAtom("stsc", body);
      expectedMessage = "MP4 sample-to-chunk entries are not ordered";
    }

    SECTION("A chunk mapping cannot contain zero samples")
    {
      stsc = ao::test::mp4::makeStscAtom(0);
      expectedMessage = "Invalid MP4 sample-to-chunk entry";
    }

    SECTION("Fixed-size sample tables cannot contain trailing entry bytes")
    {
      auto body = std::vector<std::uint8_t>{};

      // Version/flags, fixed size, count, and an extraneous entry.
      for (auto const value : {0U, 4U, 1U, 4U})
      {
        ao::test::mp4::appendBe32(body, value);
      }

      stsz = ao::test::mp4::makeAtom("stsz", body);
      expectedMessage = "Malformed fixed-size stsz atom";
    }

    SECTION("A timing entry cannot contain zero samples")
    {
      stts = ao::test::mp4::makeSttsAtom(0);
      expectedMessage = "Invalid MP4 time-to-sample entry";
    }

    auto const stbl = makeSampleTable(makeAlacStsd(), stsz, stsc, ao::test::mp4::makeStcoAtom(), stts);
    auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
    auto const res = Demuxer::parse(fileData, "alac");

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::FormatRejected);
    CHECK(res.error().message == expectedMessage);
  }

  TEST_CASE("MP4 Demuxer - preserves packets across chunk runs and missing payload", "[media][regression][mp4]")
  {
    auto stscBody = std::vector<std::uint8_t>{};

    for (auto const value : {0U, 2U, 1U, 2U, 1U, 2U, 1U, 1U})
    {
      ao::test::mp4::appendBe32(stscBody, value);
    }

    auto chunkBody = std::vector<std::uint8_t>{};

    for (auto const value : {0U, 2U, 8U, 14U})
    {
      ao::test::mp4::appendBe32(chunkBody, value);
    }

    auto sttsBody = std::vector<std::uint8_t>{};

    for (auto const value : {0U, 2U, 2U, 1024U, 1U, 512U})
    {
      ao::test::mp4::appendBe32(sttsBody, value);
    }

    auto stsz = ao::test::mp4::makeStszAtom(2, 3);
    auto stts = ao::test::mp4::makeAtom("stts", sttsBody);
    auto stsc = ao::test::mp4::makeAtom("stsc", stscBody);
    auto chunkOffsets = ao::test::mp4::makeAtom("stco", chunkBody);
    bool missingPayload = false;
    bool hasTiming = true;

    SECTION("Compact sample tables")
    {
    }

    SECTION("Extended sample tables")
    {
      stsz = ao::test::mp4::makeExtendedFromCompactAtom(stsz);
      stts = ao::test::mp4::makeExtendedFromCompactAtom(stts);
      stsc = ao::test::mp4::makeExtendedFromCompactAtom(stsc);
      chunkOffsets = ao::test::mp4::makeExtendedFromCompactAtom(chunkOffsets);
    }

    SECTION("Explicit sample sizes retain their byte-backed count")
    {
      auto body = std::vector<std::uint8_t>{};

      for (auto const value : {0U, 0U, 3U, 2U, 2U, 2U})
      {
        ao::test::mp4::appendBe32(body, value);
      }

      stsz = ao::test::mp4::makeAtom("stsz", body);
    }

    SECTION("Timing remains optional")
    {
      hasTiming = false;
      stts.clear();
    }

    SECTION("Missing packet bytes remain an empty payload")
    {
      missingPayload = true;
      auto co64Body = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(co64Body, 0);
      ao::test::mp4::appendBe32(co64Body, 2);
      ao::test::mp4::appendBe64(co64Body, 8);
      ao::test::mp4::appendBe64(co64Body, 4096);
      chunkOffsets = ao::test::mp4::makeAtom("co64", co64Body);
    }

    auto const stbl = makeSampleTable(makeAlacStsd(), stsz, stsc, chunkOffsets, stts);
    auto data = ao::test::mp4::makeAtom("mdat", {1, 2, 3, 4, 0, 0, 5, 6});
    auto const moov = ao::test::mp4::makeAtom("moov", ao::test::mp4::makeTrackAtom("soun", stbl, 48000, 2560));
    data.insert(data.end(), moov.begin(), moov.end());
    auto const fileData = toBytes(data);
    auto const res = Demuxer::parse(fileData, "alac");
    REQUIRE(res);
    REQUIRE(res->sampleCount() == 3);
    CHECK(res->sampleInfo(0).offset == 8);
    CHECK(res->sampleInfo(1).offset == 10);
    CHECK(res->sampleInfo(2).offset == (missingPayload ? 4096U : 14U));
    CHECK(res->sampleInfo(2).size == 2);
    CHECK(res->sampleInfo(2).startTime == (hasTiming ? 2048U : 0U));
    CHECK(res->sampleInfo(2).duration == (hasTiming ? 512U : 0U));
    CHECK(res->sampleIndexAtTime(2560) == 3);
    REQUIRE(res->samplePayload(0).size() == 2);
    CHECK(res->samplePayload(0)[0] == std::byte{1});
    REQUIRE(res->samplePayload(1).size() == 2);
    CHECK(res->samplePayload(1)[1] == std::byte{4});

    if (missingPayload)
    {
      CHECK(res->samplePayload(2).empty());
    }
    else
    {
      REQUIRE(res->samplePayload(2).size() == 2);
      CHECK(res->samplePayload(2)[0] == std::byte{5});
      CHECK(res->samplePayload(2)[1] == std::byte{6});
    }
  }

  TEST_CASE("MP4 Demuxer - rejects malformed sample-table inputs", "[media][unit][mp4][error]")
  {
    SECTION("Empty data returns FormatRejected")
    {
      auto const emptyData = std::vector<std::byte>{};
      auto const res = Demuxer::parse(emptyData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Small garbage data returns FormatRejected")
    {
      auto const garbage = std::array{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
      auto const res = Demuxer::parse(garbage, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
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

      auto const res = Demuxer::parse(data, "alac");

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Short audio sample entry returns FormatRejected")
    {
      auto const shortEntry = ao::test::mp4::makeAtom("alac", {});
      auto const stsd = ao::test::mp4::makeStsdAtomFromSampleEntry(shortEntry);
      auto const stbl = ao::test::mp4::makeSampleTableAtom(stsd);
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Multiple sample descriptions are not selected implicitly")
    {
      auto const alacConfig = ao::test::mp4::makeAtom("alac", {9, 8, 7});
      auto const alacEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", alacConfig);
      auto const mp4aEntry = ao::test::mp4::makeAudioSampleEntryAtom("mp4a", ao::test::mp4::makeAtom("esds", {}));
      auto const stsd = ao::test::mp4::makeStsdAtomFromSampleEntries({alacEntry, mp4aEntry});
      auto const stbl = ao::test::mp4::makeSampleTableAtom(stsd);
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
    }

    SECTION("Unsupported sample description mappings are rejected")
    {
      auto const stsc = ao::test::mp4::makeStscAtom(1, 2);
      auto const stbl =
        makeSampleTable(makeAlacStsd(), ao::test::mp4::makeStszAtom(4), stsc, ao::test::mp4::makeStcoAtom());
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message == "Invalid MP4 sample-to-chunk entry");
    }

    SECTION("First sample-to-chunk entry must start at the first chunk")
    {
      auto const stsc = ao::test::mp4::makeStscAtom(1, 1, 2);
      auto const stbl =
        makeSampleTable(makeAlacStsd(), ao::test::mp4::makeStszAtom(4), stsc, ao::test::mp4::makeStcoAtom());
      auto const fileData = makeFile(ao::test::mp4::makeTrackAtom("soun", stbl));
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message == "MP4 sample-to-chunk entry references an invalid chunk");
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
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message == "Malformed stsz entry table");
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
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::FormatRejected);
      CHECK(res.error().message == "MP4 sample offset overflow");
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

    SECTION("Unrelated video track has a huge fixed-size sample count")
    {
      auto const videoStbl = makeSampleTable(ao::test::mp4::makeStsdAtom("avc1"),
                                             ao::test::mp4::makeStszAtom(1, std::numeric_limits<std::uint32_t>::max()),
                                             ao::test::mp4::makeStcoAtom());
      auto moovBody = ao::test::mp4::makeTrackAtom("vide", videoStbl);
      moovBody.insert(moovBody.end(), track.begin(), track.end());
      auto const fileData = toBytes(ao::test::mp4::makeAtom("moov", moovBody));
      auto const res = Demuxer::parse(fileData, "alac");
      REQUIRE(res);
      CHECK(res->sampleCount() == 1);
      CHECK(res->sampleInfo(0).size == 7);
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

    auto const res = Demuxer::parse(mappedFile.bytes(), "mp4a");

    REQUIRE(res);
    CHECK_FALSE(res->magicCookie().empty());
    CHECK(res->sampleCount() > 0);
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
    auto const res = Demuxer::parse(fileData, "mp4a");

    REQUIRE(res);
    CHECK(res->timescale() == 48000);
    CHECK(res->duration() == 96000);
    CHECK(res->sampleCount() == 1);
    CHECK(res->sampleInfo(0).offset == 321);
    CHECK(res->sampleInfo(0).size == 7);
    CHECK(res->sampleInfo(0).duration == 2048);
    REQUIRE(res->magicCookie().size() == 2);
    CHECK(res->magicCookie()[0] == std::byte{0x12});
    CHECK(res->magicCookie()[1] == std::byte{0x10});
  }

  TEST_CASE("MP4 Demuxer - seek lookup preserves packet-boundary semantics", "[media][regression][mp4][seek]")
  {
    constexpr std::uint32_t kSampleCount = 8;
    constexpr std::uint32_t kSampleDelta = 1024;
    constexpr std::uint64_t kTotalDuration = static_cast<std::uint64_t>(kSampleCount) * kSampleDelta;
    auto body = std::vector<std::uint8_t>{};
    auto const stsd = makeAlacStsd();
    auto const stsz = ao::test::mp4::makeStszAtom(4, kSampleCount);
    auto const stts = ao::test::mp4::makeSttsAtom(kSampleCount, kSampleDelta);
    auto const stsc = ao::test::mp4::makeStscAtom(kSampleCount);
    auto const stco = ao::test::mp4::makeStcoAtom();
    body.insert(body.end(), stsd.begin(), stsd.end());
    body.insert(body.end(), stsz.begin(), stsz.end());
    body.insert(body.end(), stts.begin(), stts.end());
    body.insert(body.end(), stsc.begin(), stsc.end());
    body.insert(body.end(), stco.begin(), stco.end());
    auto const stbl = ao::test::mp4::makeAtom("stbl", body);
    auto const track = ao::test::mp4::makeTrackAtom("soun", stbl, 48000, kTotalDuration);
    auto fileData = makeFile(track);
    // Supply the fixed-size index budget so this fixture isolates seek timing.
    fileData.append_range(
      toBytes(ao::test::mp4::makeAtom("mdat", std::vector<std::uint8_t>(kSampleCount * sizeof(Demuxer::SampleEntry)))));
    auto const res = Demuxer::parse(fileData, "alac");

    REQUIRE(res);
    CHECK(res->sampleIndexAtTime(0) == 0);
    CHECK(res->sampleIndexAtTime(kSampleDelta - 1) == 0);
    CHECK(res->sampleIndexAtTime(kSampleDelta) == 1);
    CHECK(res->sampleIndexAtTime(kTotalDuration - 1) == kSampleCount - 1);
    CHECK(res->sampleIndexAtTime(kTotalDuration) == kSampleCount);
    CHECK(res->sampleIndexAtTime(std::numeric_limits<std::uint64_t>::max()) == kSampleCount);
  }
} // namespace ao::media::mp4::test
