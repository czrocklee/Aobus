// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/ogg/PageLayout.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/file/TestFile.h"
#include "test/unit/media/ogg/TestOgg.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>
#include <ao/media/opus/Header.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::opus::test
{
  using File = ao::media::file::test::TestFile;
  using namespace ao::media::opus;
  using namespace ao::test;

  namespace
  {
    constexpr std::string_view kVendor = "Aobus test";

    std::vector<std::uint8_t> makeHeadPacket(std::uint8_t channels = 2,
                                             std::uint8_t channelMappingFamily = kMappingFamilyMonoStereo)
    {
      auto packet = std::vector<std::uint8_t>{};

      for (char const character : std::string_view{"OpusHead"})
      {
        packet.push_back(static_cast<std::uint8_t>(character));
      }

      packet.push_back(1);                                   // version
      packet.push_back(channels);                            // channel count
      packet.push_back(0x38);                                // pre-skip low byte (312)
      packet.push_back(0x01);                                // pre-skip high byte
      packet.insert(packet.end(), {0x80, 0xBB, 0x00, 0x00}); // input sample rate 48000
      packet.insert(packet.end(), {0x00, 0x00});             // output gain
      packet.push_back(channelMappingFamily);
      return packet;
    }

    void appendLe32(std::vector<std::uint8_t>& output, std::uint32_t const value)
    {
      for (std::size_t index = 0; index < sizeof(value); ++index)
      {
        output.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU));
      }
    }

    std::vector<std::uint8_t> makeTagsPacket(std::span<std::string_view const> comments)
    {
      auto packet = std::vector<std::uint8_t>{};

      for (char const character : std::string_view{"OpusTags"})
      {
        packet.push_back(static_cast<std::uint8_t>(character));
      }

      appendLe32(packet, static_cast<std::uint32_t>(kVendor.size()));

      for (char const character : kVendor)
      {
        packet.push_back(static_cast<std::uint8_t>(character));
      }

      appendLe32(packet, static_cast<std::uint32_t>(comments.size()));

      for (auto const comment : comments)
      {
        appendLe32(packet, static_cast<std::uint32_t>(comment.size()));

        for (char const character : comment)
        {
          packet.push_back(static_cast<std::uint8_t>(character));
        }
      }

      return packet;
    }

    // A SILK table of contents selecting one 60 ms frame, so a synthetic packet
    // has the decoded length its page granule positions claim for it.
    constexpr std::uint8_t kSyntheticPacketToc = 0x18;
    constexpr std::int64_t kSyntheticPacketFrames = 2880;

    // Enough 60 ms packets to reach the default final granule position, whose
    // last page is trimmed back to it the way an encoder trims its own tail.
    constexpr std::size_t kSyntheticAudioPackets = 17;

    struct StreamSpec final
    {
      std::vector<std::uint8_t> headPacket = makeHeadPacket();
      std::optional<std::vector<std::uint8_t>> optTagsPacket = makeTagsPacket({});
      std::size_t audioPacketCount = kSyntheticAudioPackets;
      std::int64_t finalGranulePosition = 48312;
    };

    // One packet per page, matching how a real Opus file lays out its headers.
    std::vector<std::uint8_t> makeStreamBytes(StreamSpec const& spec)
    {
      auto pages = std::vector<ao::test::ogg::Page>{};
      std::uint32_t sequence = 0;

      auto const appendPage = [&pages, &sequence](
                                std::vector<std::uint8_t> packet, std::uint8_t headerType, std::int64_t granulePosition)
      {
        pages.push_back(ao::test::ogg::Page{.headerType = headerType,
                                            .granulePosition = granulePosition,
                                            .pageSequence = sequence,
                                            .lacingValues = ao::test::ogg::lacingFor({packet.size()}),
                                            .payload = std::move(packet)});
        ++sequence;
      };

      auto const totalPages = std::size_t{1} + (spec.optTagsPacket ? 1 : 0) + spec.audioPacketCount;
      auto const lastFlag = [totalPages](std::size_t index)
      { return index + 1 == totalPages ? ogg::kEndOfStreamFlag : std::uint8_t{0}; };

      appendPage(spec.headPacket, ogg::kBeginOfStreamFlag | lastFlag(0), 0);

      if (spec.optTagsPacket)
      {
        appendPage(*spec.optTagsPacket, lastFlag(pages.size()), 0);
      }

      for (std::size_t index = 0; index < spec.audioPacketCount; ++index)
      {
        auto const isLast = index + 1 == spec.audioPacketCount;
        auto const decoded = static_cast<std::int64_t>(index + 1) * kSyntheticPacketFrames;
        auto packet = std::vector<std::uint8_t>(32, 0x00);
        packet[0] = kSyntheticPacketToc;
        appendPage(std::move(packet),
                   lastFlag(pages.size()),
                   isLast ? spec.finalGranulePosition : std::min(decoded, spec.finalGranulePosition));
      }

      auto bytes = std::vector<std::uint8_t>{};

      for (auto const& page : pages)
      {
        auto const pageBytes = ao::test::ogg::makePage(page);
        bytes.insert(bytes.end(), pageBytes.begin(), pageBytes.end());
      }

      return bytes;
    }

    ao::media::file::test::RecordedContent readContent(File const& file)
    {
      auto result = file.readContent();
      REQUIRE(result);
      return *result;
    }

    bool containsBytes(std::span<std::byte const> haystack, std::string_view needle)
    {
      return utility::bytes::stringView(haystack).contains(needle);
    }
  } // namespace

  TEST_CASE("Opus File - emits real fixture tag fields", "[media][unit][opus][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.opus")};
    auto const content = readContent(file);

    CHECK(content.text(TextField::Title) == "Test Title");
    CHECK(content.text(TextField::Artist) == "Test Artist");
    CHECK(content.text(TextField::Album) == "Test Album");
    CHECK(content.text(TextField::Genre) == "Rock");
    CHECK(content.text(TextField::Composer) == "Test Composer");
    CHECK(content.text(TextField::Work) == "Symphony No. 5");
    CHECK(content.number(NumberField::TrackNumber) == 1);
    CHECK(content.number(NumberField::Year) == 2024);
  }

  TEST_CASE("Opus File - reports the decoded signal rather than the encoder input", "[media][unit][opus][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.opus")};
    auto const content = readContent(file);

    CHECK(content.codec() == AudioCodec::Opus);
    CHECK(content.sampleRate() == kDecodedSampleRate);
    CHECK(content.channels() == 2);
    CHECK(content.bitrate() > Bitrate{0});

    // Granule positions include the pre-skip the decoder discards, so the
    // reported length is the audible one.
    CHECK(content.duration() == std::chrono::seconds{1});

    // Opus carries no sample depth, so the reader must not invent one.
    CHECK(content.bitDepth() == BitDepth{});
  }

  TEST_CASE("Opus File - reports a mono stream as one channel", "[media][unit][opus][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("mono.opus")};
    auto const content = readContent(file);

    CHECK(content.channels() == 1);
    CHECK(content.text(TextField::Title) == "Mono Title");
  }

  TEST_CASE("Opus File - maps classical comments and their fallbacks", "[media][unit][opus][file]")
  {
    SECTION("Primary classical fields")
    {
      auto const file = File{audio::test::requireAudioFixture("classical_metadata.opus")};
      auto const content = readContent(file);

      CHECK(content.text(TextField::Conductor) == "Fixture Conductor");
      CHECK(content.text(TextField::Ensemble) == "Fixture Ensemble");
      CHECK(content.text(TextField::Soloist) == "Fixture Soloist");
      CHECK(content.text(TextField::Work) == "Fixture Work");
      CHECK(content.text(TextField::Movement) == "Fixture Movement");
      CHECK(content.number(NumberField::MovementNumber) == 2);
      CHECK(content.number(NumberField::MovementTotal) == 4);
      CHECK(content.number(NumberField::TrackNumber) == 3);
      CHECK(content.number(NumberField::TrackTotal) == 9);
      CHECK(content.number(NumberField::Year) == 2026);
    }

    SECTION("Fallback aliases fill absent primary fields")
    {
      auto const file = File{audio::test::requireAudioFixture("classical_fallback.opus")};
      auto const content = readContent(file);

      CHECK(content.text(TextField::Title) == "Classical Fallback");
      CHECK(content.text(TextField::Ensemble) == "Fixture Fallback Ensemble");
      CHECK(content.text(TextField::Soloist) == "Fixture Fallback Soloist");
    }
  }

  TEST_CASE("Opus File - imports a Base64 picture comment", "[media][unit][opus][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("with_cover.opus")};
    auto const content = readContent(file);

    REQUIRE(content.pictures().size() == 1);
    auto const& picture = content.pictures().front();
    CHECK(picture.type == PictureType::FrontCover);
    REQUIRE(picture.bytes.size() > 8);

    // The decoded comment must hand out the image itself, not its container.
    CHECK(containsBytes(picture.bytes.first(8), std::string_view{"PNG"}));
  }

  TEST_CASE("Opus File - ignores a Base64 picture comment with invalid structure", "[media][unit][opus][file]")
  {
    auto const comments = std::vector<std::string_view>{"METADATA_BLOCK_PICTURE=AAAA"};
    auto spec = StreamSpec{};
    spec.optTagsPacket = makeTagsPacket(comments);

    auto const temp = TempFile{makeStreamBytes(spec), ".opus"};
    auto const file = File{temp.path};
    auto const content = readContent(file);

    CHECK(content.pictures().empty());
  }

  TEST_CASE("Opus File - a tagless stream still reports its audio properties", "[media][unit][opus][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("empty.opus")};
    auto const content = readContent(file);

    CHECK(content.text(TextField::Title).empty());
    CHECK(content.text(TextField::Artist).empty());
    CHECK(content.pictures().empty());
    CHECK(content.codec() == AudioCodec::Opus);
    CHECK(content.sampleRate() == kDecodedSampleRate);
    CHECK(content.duration() == std::chrono::seconds{1});
  }

  TEST_CASE("Opus File - audio payload excludes the header pages", "[media][unit][opus][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.opus")};
    auto const payload = requireValue(file.audioPayload());

    CHECK(payload.offset > 0);
    REQUIRE(payload.bytes.size() > 4);

    // The payload must start at a page boundary and hold no tag bytes. A tag
    // edit preserves identity when it also preserves header pagination.
    CHECK(containsBytes(payload.bytes.first(4), std::string_view{"OggS"}));
    CHECK_FALSE(containsBytes(payload.bytes, std::string_view{"OpusTags"}));
    CHECK_FALSE(containsBytes(payload.bytes, std::string_view{"Test Title"}));
  }

  TEST_CASE("Opus File - rejects streams it cannot interpret", "[media][unit][opus][file]")
  {
    auto const readError = [](std::vector<std::uint8_t> const& bytes)
    {
      auto const temp = TempFile{bytes, ".opus"};
      auto const file = File{temp.path};
      auto const result = file.readContent();
      REQUIRE_FALSE(result);
      return result.error().code;
    };

    SECTION("Content that is not an Ogg stream is corrupt")
    {
      CHECK(readError(std::vector<std::uint8_t>(128, 0x41)) == Error::Code::CorruptData);
    }

    SECTION("A first packet that is not OpusHead is corrupt")
    {
      auto spec = StreamSpec{};
      spec.headPacket = makeTagsPacket({});
      CHECK(readError(makeStreamBytes(spec)) == Error::Code::CorruptData);
    }

    SECTION("A stream without a tags packet is corrupt")
    {
      auto spec = StreamSpec{};
      spec.optTagsPacket.reset();
      spec.audioPacketCount = 0;
      CHECK(readError(makeStreamBytes(spec)) == Error::Code::CorruptData);
    }

    SECTION("A stream without audio packets is corrupt")
    {
      auto spec = StreamSpec{};
      spec.audioPacketCount = 0;
      CHECK(readError(makeStreamBytes(spec)) == Error::Code::CorruptData);
    }

    SECTION("An identification packet declaring no channels is corrupt")
    {
      auto spec = StreamSpec{};
      spec.headPacket = makeHeadPacket(0);
      CHECK(readError(makeStreamBytes(spec)) == Error::Code::CorruptData);
    }

    SECTION("A channel mapping family this reader cannot describe is unsupported")
    {
      auto spec = StreamSpec{};
      spec.headPacket = makeHeadPacket(4, 2);
      CHECK(readError(makeStreamBytes(spec)) == Error::Code::NotSupported);
    }
  }

  TEST_CASE("Opus File - reads comments from a synthetic tags packet", "[media][unit][opus][file]")
  {
    auto const comments = std::vector<std::string_view>{
      "TITLE=Synthetic", "ARTIST=Synthetic Artist", "TRACKNUMBER=4/12", "UNMAPPED=ignored"};
    auto spec = StreamSpec{};
    spec.optTagsPacket = makeTagsPacket(comments);

    auto const temp = TempFile{makeStreamBytes(spec), ".opus"};
    auto const file = File{temp.path};
    auto const content = readContent(file);

    CHECK(content.text(TextField::Title) == "Synthetic");
    CHECK(content.text(TextField::Artist) == "Synthetic Artist");
    CHECK(content.number(NumberField::TrackNumber) == 4);
    CHECK(content.number(NumberField::TrackTotal) == 12);
    CHECK(content.duration() == std::chrono::seconds{1});
  }

  TEST_CASE("Opus File - maps common Vorbis comment aliases", "[media][unit][opus][file]")
  {
    auto comments = std::vector<std::string_view>{};
    auto expectedAlbumArtist = std::string_view{};
    std::uint16_t expectedYear = 0;
    std::uint16_t expectedTrackNumber = 0;
    std::uint16_t expectedTrackTotal = 0;
    std::uint16_t expectedDiscNumber = 0;
    std::uint16_t expectedDiscTotal = 0;

    SECTION("Space-separated album artist")
    {
      comments = {"YEAR=2025", "ALBUM ARTIST=Space Artist", "TRACK=4/12", "DISC=2/3"};
      expectedAlbumArtist = "Space Artist";
      expectedYear = 2025;
      expectedTrackNumber = 4;
      expectedTrackTotal = 12;
      expectedDiscNumber = 2;
      expectedDiscTotal = 3;
    }

    SECTION("Underscore-separated album artist")
    {
      comments = {"ALBUM_ARTIST=Underscore Artist"};
      expectedAlbumArtist = "Underscore Artist";
    }

    auto spec = StreamSpec{};
    spec.optTagsPacket = makeTagsPacket(comments);

    auto const temp = TempFile{makeStreamBytes(spec), ".opus"};
    auto const file = File{temp.path};
    auto const content = readContent(file);

    CHECK(content.text(TextField::AlbumArtist) == expectedAlbumArtist);
    CHECK(content.number(NumberField::Year) == expectedYear);
    CHECK(content.number(NumberField::TrackNumber) == expectedTrackNumber);
    CHECK(content.number(NumberField::TrackTotal) == expectedTrackTotal);
    CHECK(content.number(NumberField::DiscNumber) == expectedDiscNumber);
    CHECK(content.number(NumberField::DiscTotal) == expectedDiscTotal);
  }
} // namespace ao::media::file::opus::test
