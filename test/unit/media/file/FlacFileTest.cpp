// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/file/TestFile.h"
#include <ao/AudioCodec.h>
#include <ao/media/file/Visitor.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::media::file::flac::test
{
  using File = ao::media::file::test::TestFile;
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

    ao::media::file::test::RecordedContent readContent(File const& file)
    {
      auto result = file.readContent();
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("FLAC File - parses metadata and audio properties", "[media][unit][flac][file]")
  {
    auto data = createMinimalFlac();
    data.push_back(0xA0);
    auto const temp = TempFile{data, ".flac"};

    auto const file = File{temp.path};
    auto content = readContent(file);

    auto const& metadata = content;
    CHECK(metadata.text(TextField::Title) == "Title");
    CHECK(metadata.text(TextField::Artist) == "Artist");
    CHECK(metadata.text(TextField::AlbumArtist) == "AlbumArtist");
    CHECK(metadata.text(TextField::Composer) == "Composer");
    CHECK(metadata.text(TextField::Conductor) == "Conductor");
    CHECK(metadata.text(TextField::Ensemble) == "Ensemble");
    CHECK(metadata.text(TextField::Genre) == "Genre");
    CHECK(metadata.number(NumberField::TrackNumber) == 1);
    CHECK(metadata.number(NumberField::TrackTotal) == 10);
    CHECK(metadata.number(NumberField::DiscNumber) == 2);
    CHECK(metadata.number(NumberField::DiscTotal) == 5);
    CHECK(metadata.number(NumberField::Year) == 2024);
    CHECK(metadata.text(TextField::Work) == "WorkName");
    CHECK(metadata.text(TextField::Movement) == "MovementName");
    CHECK(metadata.text(TextField::Soloist) == "Soloist");
    CHECK(metadata.number(NumberField::MovementNumber) == 2);
    CHECK(metadata.number(NumberField::MovementTotal) == 4);
    auto const& prop = content;
    CHECK(prop.sampleRate() == 44100);
    CHECK(prop.channels() == 2);
    CHECK(prop.bitDepth() == 16);
    CHECK(prop.codec() == AudioCodec::Flac);
    CHECK(prop.duration() == std::chrono::seconds{1});
  }

  TEST_CASE("FLAC File - emits real fixture tag fields", "[media][unit][flac][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.flac")};
    auto content = readContent(file);
    auto const& metadata = content;

    CHECK(metadata.text(TextField::Title) == "Test Title");
    CHECK(metadata.text(TextField::Artist) == "Test Artist");
    CHECK(metadata.text(TextField::Album) == "Test Album");
    CHECK(metadata.text(TextField::Genre) == "Rock");
    CHECK(metadata.text(TextField::Composer) == "Test Composer");
    CHECK(metadata.text(TextField::Work) == "Symphony No. 5");
    CHECK(metadata.number(NumberField::TrackNumber) == 1);
    CHECK(metadata.number(NumberField::Year) == 2024);
  }

  TEST_CASE("FLAC File - maps classical fallback comments when primary fields are absent", "[media][unit][flac][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("classical_fallback.flac")};
    auto content = readContent(file);
    auto const& metadata = content;

    CHECK(metadata.text(TextField::Title) == "Classical Fallback");
    CHECK(metadata.text(TextField::Ensemble) == "Fixture Fallback Ensemble");
    CHECK(metadata.text(TextField::Soloist) == "Fixture Fallback Soloist");
  }

  TEST_CASE("FLAC File - audio payload range starts after metadata blocks", "[media][unit][flac][file]")
  {
    auto data = createMinimalFlac();
    std::size_t const expectedOffset = data.size();
    data.insert(data.end(), {0xA0, 0xA1, 0xA2});

    auto const temp = TempFile{data, ".flac"};
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

  TEST_CASE("FLAC File - rejects empty audio payload", "[media][unit][flac][file]")
  {
    auto const data = createMinimalFlac();
    auto const temp = TempFile{data, ".flac"};
    auto const file = File{temp.path};
    auto rangeResult = file.audioPayload();

    REQUIRE_FALSE(rangeResult);
    CHECK(rangeResult.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("FLAC File - audio payload signature ignores metadata changes", "[media][unit][flac][file]")
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
    CHECK(utility::xxh3Hash128(firstPayloadResult->bytes) == utility::xxh3Hash128(secondPayloadResult->bytes));
  }

  TEST_CASE("FLAC File - rejects malformed input", "[media][unit][flac][file]")
  {
    SECTION("Missing StreamInfo block")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};
      addBlockHeader(data, MetadataBlockType::VorbisComment, true, 10);
      data.insert(data.end(), 10, 0);

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
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

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
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

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
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

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
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

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("StreamInfo payload size must be exactly 34 bytes")
    {
      for (auto const declaredSize : {33U, 35U})
      {
        auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};
        addBlockHeader(data, MetadataBlockType::StreamInfo, true, declaredSize);
        data.insert(data.end(), declaredSize, 0);

        auto const temp = TempFile{data, ".flac"};
        auto const file = File{temp.path};
        auto result = file.readContent();
        REQUIRE_FALSE(result);
        CHECK(result.error().code == Error::Code::CorruptData);
      }
    }

    SECTION("Malformed bounded Vorbis comment block is discarded atomically")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      auto comments = std::vector<std::uint8_t>{0, 0, 0, 0, 2, 0, 0, 0};
      auto const firstComment = std::string_view{"TITLE=Partial"};
      comments.push_back(static_cast<std::uint8_t>(firstComment.size()));
      comments.insert(comments.end(), {0, 0, 0});
      comments.insert(comments.end(), firstComment.begin(), firstComment.end());
      comments.insert(comments.end(), {100, 0, 0, 0});
      addBlockHeader(data, MetadataBlockType::VorbisComment, true, static_cast<std::uint32_t>(comments.size()));
      data.insert(data.end(), comments.begin(), comments.end());
      data.push_back(0xA0);

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();

      REQUIRE(result);
      CHECK(result->text(TextField::Title).empty());
      CHECK(result->codec() == AudioCodec::Flac);
    }

    SECTION("Untrusted Vorbis comment count does not control allocation")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      auto const comments = std::vector<std::uint8_t>{
        0,
        0,
        0,
        0, // Empty vendor string.
        0xFF,
        0xFF,
        0xFF,
        0xFF, // Impossible comment count for this payload.
      };
      addBlockHeader(data, MetadataBlockType::VorbisComment, true, static_cast<std::uint32_t>(comments.size()));
      data.insert(data.end(), comments.begin(), comments.end());
      data.push_back(0xA0);

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();

      REQUIRE(result);
      CHECK(result->text(TextField::Title).empty());
      CHECK(result->codec() == AudioCodec::Flac);
    }

    SECTION("Malformed bounded picture block is discarded")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      addBlockHeader(data, MetadataBlockType::Picture, true, 4);
      data.insert(data.end(), 4, 0);
      data.push_back(0xA0);

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
      REQUIRE(result);
      CHECK(result->pictures().empty());
      CHECK(result->codec() == AudioCodec::Flac);
    }

    SECTION("Structurally valid empty picture is omitted")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

      addBlockHeader(data, MetadataBlockType::StreamInfo, false, 34);
      auto si = StreamInfoLayout{};
      auto const* siAddr = reinterpret_cast<std::uint8_t const*>(&si);
      data.insert(data.end(), siAddr, siAddr + 34);

      // Type, empty MIME, empty description, four geometry scalars, empty image.
      auto picture = std::vector<std::uint8_t>(32, 0);
      addBlockHeader(data, MetadataBlockType::Picture, true, static_cast<std::uint32_t>(picture.size()));
      data.insert(data.end(), picture.begin(), picture.end());
      data.push_back(0xA0);

      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();

      REQUIRE(result);
      CHECK(result->pictures().empty());
      CHECK(result->codec() == AudioCodec::Flac);
    }

    SECTION("Invalid FLAC magic signature")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'K'};
      auto const temp = TempFile{data, ".flac"};
      auto const file = File{temp.path};
      auto result = file.readContent();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::media::file::flac::test
