// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/flac/File.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/AudioCodec.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/utility/Fnv1a.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tag::flac::test
{
  using namespace ao::media::flac;
  using namespace ao::test;

  namespace
  {
    void addBlockHeader(std::vector<std::uint8_t>& data, MetadataBlockType type, bool isLast, std::uint32_t size)
    {
      auto first = static_cast<std::uint8_t>(type);

      if (isLast)
      {
        first |= 0x80;
      }

      data.push_back(first);
      data.push_back((size >> 16) & 0xFF);
      data.push_back((size >> 8) & 0xFF);
      data.push_back(size & 0xFF);
    }

    std::vector<std::uint8_t> createMinimalFlac(std::string_view title = "Title")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      // StreamInfo block
      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      si.packedFields = (44100ULL << 44) | (1ULL << 41) | (15ULL << 36) | 44100ULL;
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      // Vorbis Comment block
      auto vc = std::vector<std::uint8_t>{};
      auto addString = [&](std::string_view s)
      {
        std::uint32_t const len = static_cast<std::uint32_t>(s.size());
        vc.push_back(len & 0xFF);
        vc.push_back((len >> 8) & 0xFF);
        vc.push_back((len >> 16) & 0xFF);
        vc.push_back((len >> 24) & 0xFF);
        vc.insert(vc.end(), s.begin(), s.end());
      };
      addString("Vendor");
      std::uint32_t const count = 21;
      vc.push_back(count & 0xFF);
      vc.push_back((count >> 8) & 0xFF);
      vc.push_back((count >> 16) & 0xFF);
      vc.push_back((count >> 24) & 0xFF);
      auto titleComment = std::string{"TITLE="};
      titleComment += title;
      addString(titleComment);
      addString("ARTIST=Artist");
      addString("ALBUMARTIST=AlbumArtist");
      addString("COMPOSER=Composer");
      addString("CONDUCTOR=Conductor");
      addString("ENSEMBLE=Ensemble");
      addString("ORCHESTRA=Orchestra Fallback");
      addString("GENRE=Genre");
      addString("TRACKNUMBER=1");
      addString("TRACKTOTAL=10");
      addString("TOTALTRACKS=10");
      addString("DISCNUMBER=2/5");
      addString("DISCTOTAL=5");
      addString("TOTALDISCS=5");
      addString("DATE=2024");
      addString("WORK=WorkName");
      addString("MOVEMENTNAME=MovementName");
      addString("MOVEMENT=2/4");
      addString("SOLOIST=Soloist");
      addString("PERFORMER=Performer Fallback");
      addString("UNKNOWN=IgnoredValue");

      addBlockHeader(data, MetadataBlockType::VorbisComment, true, static_cast<std::uint32_t>(vc.size()));
      data.insert(data.end(), vc.begin(), vc.end());

      return data;
    }

    library::TrackBuilder loadTrack(File const& file)
    {
      auto result = file.loadTrack();
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("FLAC File - parses metadata and audio properties", "[tag][unit][flac][file]")
  {
    auto const data = createMinimalFlac();
    auto const temp = TempFile{data};

    auto const file = File{temp.path};
    auto builder = loadTrack(file);

    auto const meta = builder.metadata();
    CHECK(meta.title() == "Title");
    CHECK(meta.artist() == "Artist");
    CHECK(meta.albumArtist() == "AlbumArtist");
    CHECK(meta.composer() == "Composer");
    CHECK(meta.conductor() == "Conductor");
    CHECK(meta.ensemble() == "Ensemble");
    CHECK(meta.genre() == "Genre");
    CHECK(meta.trackNumber() == 1);
    CHECK(meta.trackTotal() == 10);
    CHECK(meta.discNumber() == 2);
    CHECK(meta.discTotal() == 5);
    CHECK(meta.year() == 2024);
    CHECK(meta.work() == "WorkName");
    CHECK(meta.movement() == "MovementName");
    CHECK(meta.soloist() == "Soloist");
    CHECK(meta.movementNumber() == 2);
    CHECK(meta.movementTotal() == 4);
    CHECK(builder.customMetadata().pairs().empty());

    auto const prop = builder.property();
    CHECK(prop.sampleRate() == 44100);
    CHECK(prop.channels() == 2);
    CHECK(prop.bitDepth() == 16);
    CHECK(prop.codec() == AudioCodec::Flac);
    CHECK(prop.duration() == std::chrono::seconds{1});
  }

  TEST_CASE("FLAC File - maps real fixture tags into TrackBuilder", "[tag][unit][flac][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.flac")};
    auto builder = loadTrack(file);
    auto const meta = builder.metadata();

    CHECK(meta.title() == "Test Title");
    CHECK(meta.artist() == "Test Artist");
    CHECK(meta.album() == "Test Album");
    CHECK(meta.genre() == "Rock");
    CHECK(meta.composer() == "Test Composer");
    CHECK(meta.work() == "Symphony No. 5");
    CHECK(meta.trackNumber() == 1);
    CHECK(meta.year() == 2024);
  }

  TEST_CASE("FLAC File - maps classical fallback comments when primary fields are absent", "[tag][unit][flac][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("classical_fallback.flac")};
    auto builder = loadTrack(file);
    auto const meta = builder.metadata();

    CHECK(meta.title() == "Classical Fallback");
    CHECK(meta.ensemble() == "Fixture Fallback Ensemble");
    CHECK(meta.soloist() == "Fixture Fallback Soloist");
  }

  TEST_CASE("FLAC File - audio payload range starts after metadata blocks", "[tag][unit][flac][file]")
  {
    auto data = createMinimalFlac();
    std::size_t const expectedOffset = data.size();
    data.insert(data.end(), {0xA0, 0xA1, 0xA2});

    auto const temp = TempFile{data};
    auto const file = File{temp.path};
    auto rangeResult = file.audioPayload();

    REQUIRE(rangeResult);
    auto const range = *rangeResult;
    CHECK(range.offset == expectedOffset);
    REQUIRE(range.bytes.size() == 3);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[0]) == 0xA0);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[1]) == 0xA1);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[2]) == 0xA2);
  }

  TEST_CASE("FLAC File - rejects empty audio payload", "[tag][unit][flac][file]")
  {
    auto const data = createMinimalFlac();
    auto const temp = TempFile{data};
    auto const file = File{temp.path};
    auto rangeResult = file.audioPayload();

    REQUIRE_FALSE(rangeResult);
    CHECK(rangeResult.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("FLAC File - audio payload signature ignores metadata changes", "[tag][unit][flac][file]")
  {
    auto firstData = createMinimalFlac("Short");
    auto secondData = createMinimalFlac("A Longer Retagged Title");
    auto const audioPayload = std::vector<std::uint8_t>{0xC0, 0xC1, 0xC2, 0xC3};
    firstData.insert(firstData.end(), audioPayload.begin(), audioPayload.end());
    secondData.insert(secondData.end(), audioPayload.begin(), audioPayload.end());

    auto const firstTemp = TempFile{firstData, ".flac"};
    auto const secondTemp = TempFile{secondData, ".flac"};
    auto const firstFile = File{firstTemp.path};
    auto const secondFile = File{secondTemp.path};

    auto const firstPayloadResult = firstFile.audioPayload();
    auto const secondPayloadResult = secondFile.audioPayload();
    REQUIRE(firstPayloadResult);
    REQUIRE(secondPayloadResult);

    CHECK(firstPayloadResult->bytes.size() == audioPayload.size());
    CHECK(secondPayloadResult->bytes.size() == audioPayload.size());
    CHECK(utility::fnv1a128(firstPayloadResult->bytes) == utility::fnv1a128(secondPayloadResult->bytes));
  }

  TEST_CASE("FLAC File - rejects malformed input", "[tag][unit][flac][file]")
  {
    SECTION("Missing StreamInfo block")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};
      addBlockHeader(data, MetadataBlockType::VorbisComment, true, 10);
      data.insert(data.end(), 10, 0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Block size exceeds file boundary")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      // 1. Valid StreamInfo block
      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      // 2. Out-of-bounds VorbisComment block (not the last block so increment runs)
      addBlockHeader(data, MetadataBlockType::VorbisComment, false, 1000);
      data.insert(data.end(), 34, 0); // Only provide 34 bytes

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Trailing bytes too small for the next block header")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      // Valid StreamInfo, not marked last, so the iterator advances.
      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      // Only two trailing bytes follow - not enough for a 4-byte block header.
      data.push_back(0);
      data.push_back(0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Last metadata block cannot extend past file boundary")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      addBlockHeader(data, MetadataBlockType::Padding, true, 1000);
      data.insert(data.end(), 4, 0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Current last block size is validated before the last-block marker is honored")
    {
      // A single StreamInfo reaches the iterator as the current block and is marked
      // last, but its declared size runs past the file. The current block's size must
      // be validated before the last-block marker is honored; otherwise the iterator
      // would terminate cleanly and silently accept the truncated block.
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, true, 1000);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Picture block is truncated after picture type")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      addBlockHeader(data, MetadataBlockType::Picture, true, 4);
      data.insert(data.end(), 4, 0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Invalid FLAC magic signature")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'K'};
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::tag::flac::test
