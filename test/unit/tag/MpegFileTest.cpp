// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/mpeg/File.h"
#include "lib/tag/mpeg/FrameLayout.h"
#include "lib/tag/mpeg/id3v2/Layout.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/AudioCodec.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tag::mpeg::test
{
  using namespace ao::test;

  namespace
  {
    void addTextFrame(std::vector<std::uint8_t>& data, char const* id, std::string_view text)
    {
      auto frame = id3v2::V23TextFrameLayout{};
      std::memcpy(frame.common.id.data(), id, 4);
      frame.common.size = static_cast<std::uint32_t>(text.size() + 1);
      frame.encoding = id3v2::Encoding::Latin1;
      auto const* ptr = reinterpret_cast<std::uint8_t const*>(&frame);
      data.insert(data.end(), ptr, ptr + sizeof(frame));
      data.insert(data.end(), text.begin(), text.end());
    }

    void addTxxxFrame(std::vector<std::uint8_t>& data, std::string_view description, std::string_view value)
    {
      auto frame = id3v2::V23TextFrameLayout{};
      std::memcpy(frame.common.id.data(), "TXXX", 4);
      frame.common.size = static_cast<std::uint32_t>(1 + description.size() + 1 + value.size());
      frame.encoding = id3v2::Encoding::Latin1;
      auto const* ptr = reinterpret_cast<std::uint8_t const*>(&frame);
      data.insert(data.end(), ptr, ptr + sizeof(frame));
      data.insert(data.end(), description.begin(), description.end());
      data.push_back(0);
      data.insert(data.end(), value.begin(), value.end());
    }

    void addPictureFrame(std::vector<std::uint8_t>& data,
                         std::uint8_t pictureType,
                         std::array<std::uint8_t, 2> imageData)
    {
      auto body = std::vector<std::uint8_t>{};
      body.push_back(0); // Latin1
      body.insert(body.end(), {'i', 'm', 'g', '/', 'j', 'p', 'e', 'g', 0});
      body.push_back(pictureType);
      body.push_back(0); // Empty description
      body.insert(body.end(), imageData.begin(), imageData.end());

      auto header = id3v2::V23CommonFrameLayout{};
      std::memcpy(header.id.data(), "APIC", 4);
      header.size = static_cast<std::uint32_t>(body.size());
      data.insert(data.end(),
                  reinterpret_cast<std::uint8_t const*>(&header),
                  reinterpret_cast<std::uint8_t const*>(&header) + sizeof(header));
      data.insert(data.end(), body.begin(), body.end());
    }

    // Encode a 28-bit value into a 4-byte syncsafe integer (ID3v2.4 frame sizes).
    void putSyncSafeSize(id3v2::EncodedSize& out, std::uint32_t value)
    {
      out.data[0] = (value >> 21) & 0x7F;
      out.data[1] = (value >> 14) & 0x7F;
      out.data[2] = (value >> 7) & 0x7F;
      out.data[3] = value & 0x7F;
    }

    void addV24TextFrame(std::vector<std::uint8_t>& data,
                         char const* id,
                         id3v2::Encoding encoding,
                         std::span<std::uint8_t const> text)
    {
      auto common = id3v2::V24CommonFrameLayout{};
      std::memcpy(common.id.data(), id, 4);
      putSyncSafeSize(common.size, static_cast<std::uint32_t>(1 + text.size())); // encoding byte + text
      auto const* ptr = reinterpret_cast<std::uint8_t const*>(&common);
      data.insert(data.end(), ptr, ptr + sizeof(common));
      data.push_back(static_cast<std::uint8_t>(encoding));
      data.insert(data.end(), text.begin(), text.end());
    }

    // Wrap an ID3v2 tag body in a header of the given major version (header size is
    // always syncsafe regardless of version).
    std::vector<std::uint8_t> wrapId3v2(std::uint8_t majorVersion, std::vector<std::uint8_t> const& body)
    {
      auto data = std::vector<std::uint8_t>{};
      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", 3);
      header.majorVersion = majorVersion;

      auto const sz = static_cast<std::uint32_t>(body.size());
      header.size.data[0] = (sz >> 21) & 0x7F;
      header.size.data[1] = (sz >> 14) & 0x7F;
      header.size.data[2] = (sz >> 7) & 0x7F;
      header.size.data[3] = sz & 0x7F;

      auto const* hdr = reinterpret_cast<std::uint8_t const*>(&header);
      data.insert(data.end(), hdr, hdr + 10);
      data.insert(data.end(), body.begin(), body.end());
      return data;
    }

    std::span<std::uint8_t const> asBytes(std::string_view str)
    {
      return {reinterpret_cast<std::uint8_t const*>(str.data()), str.size()};
    }

    std::vector<std::uint8_t> createMp3WithTags()
    {
      auto body = std::vector<std::uint8_t>{};
      addTextFrame(body, "TIT2", "Title");
      addTextFrame(body, "TPE1", "Artist");
      addTextFrame(body, "TPE3", "Conductor");
      addTextFrame(body, "TALB", "Album");
      addTextFrame(body, "TYER", "2024");
      addTextFrame(body, "TRCK", "1/10");
      addTextFrame(body, "TCON", "Genre");
      addTextFrame(body, "MVNM", "MovementName");
      addTextFrame(body, "MVIN", "2/4");

      addTxxxFrame(body, "work", "WorkName");
      addTxxxFrame(body, "ensemble", "Ensemble");
      addTxxxFrame(body, "orchestra", "Orchestra Fallback");
      addTxxxFrame(body, "soloist", "Soloist");
      addTxxxFrame(body, "CustomKey", "CustomValue");

      addPictureFrame(body, 3, {0xAA, 0xBB}); // Front cover
      addPictureFrame(body, 4, {0xCC, 0xDD}); // Back cover

      // Padding
      body.insert(body.end(), 10, 0);

      auto data = std::vector<std::uint8_t>{};
      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", 3);
      header.majorVersion = 3;

      std::uint32_t const sz = static_cast<std::uint32_t>(body.size());
      header.size.data[0] = (sz >> 21) & 0x7F;
      header.size.data[1] = (sz >> 14) & 0x7F;
      header.size.data[2] = (sz >> 7) & 0x7F;
      header.size.data[3] = sz & 0x7F;

      data.insert(data.end(),
                  reinterpret_cast<std::uint8_t const*>(&header),
                  reinterpret_cast<std::uint8_t const*>(&header) + 10);
      data.insert(data.end(), body.begin(), body.end());

      // MPEG frame with Xing header
      auto frame = std::vector<std::uint8_t>(417, 0);
      frame[0] = 0xFF;
      frame[1] = 0xFB;
      frame[2] = 0x90;
      frame[3] = 0x44;

      auto* xing = reinterpret_cast<XingLayout*>(frame.data() + 36);
      std::memcpy(xing->magic.data(), "Xing", 4);
      xing->flags = 0x01 | 0x02;
      auto* frames = reinterpret_cast<boost::endian::big_uint32_buf_t*>(frame.data() + 36 + 8);
      *frames = 100;
      auto* bytes = reinterpret_cast<boost::endian::big_uint32_buf_t*>(frame.data() + 36 + 12);
      *bytes = 100 * 417;

      data.insert(data.end(), frame.begin(), frame.end());

      // ID3v1 tag
      auto id3v1 = std::vector<std::uint8_t>(128, 0);
      std::memcpy(id3v1.data(), "TAG", 3);
      data.insert(data.end(), id3v1.begin(), id3v1.end());

      return data;
    }

    std::vector<std::uint8_t> createValidMpegFrame()
    {
      auto frame = std::vector<std::uint8_t>(417, 0);
      frame[0] = 0xFF;
      frame[1] = 0xFB;
      frame[2] = 0x90;
      frame[3] = 0x44;
      return frame;
    }

    void appendId3v1Tag(std::vector<std::uint8_t>& data)
    {
      auto tag = std::vector<std::uint8_t>(128, 0);
      std::memcpy(tag.data(), "TAG", 3);
      data.insert(data.end(), tag.begin(), tag.end());
    }

    void appendApeV2Footer(std::vector<std::uint8_t>& data)
    {
      auto footer = std::vector<std::uint8_t>(32, 0);
      std::memcpy(footer.data(), "APETAGEX", 8);
      footer[8] = 0xD0;
      footer[9] = 0x07;
      footer[12] = 32;
      data.insert(data.end(), footer.begin(), footer.end());
    }

    void appendApeV2HeaderAndFooter(std::vector<std::uint8_t>& data)
    {
      auto header = std::vector<std::uint8_t>(32, 0);
      std::memcpy(header.data(), "APETAGEX", 8);
      header[8] = 0xD0;
      header[9] = 0x07;
      header[12] = 32;
      data.insert(data.end(), header.begin(), header.end());

      auto footer = std::vector<std::uint8_t>(32, 0);
      std::memcpy(footer.data(), "APETAGEX", 8);
      footer[8] = 0xD0;
      footer[9] = 0x07;
      footer[12] = 32;
      footer[23] = 0x80;
      data.insert(data.end(), footer.begin(), footer.end());
    }

    library::TrackBuilder loadTrack(File const& file)
    {
      auto result = file.loadTrack();
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("MPEG File - parses metadata and cover art", "[tag][unit][mpeg][file]")
  {
    auto const data = createMp3WithTags();
    auto const temp = TempFile{data};

    auto const file = File{temp.path};
    auto builder = loadTrack(file);

    auto const meta = builder.metadata();
    CHECK(meta.title() == "Title");
    CHECK(meta.artist() == "Artist");
    CHECK(meta.album() == "Album");
    CHECK(meta.conductor() == "Conductor");
    CHECK(meta.year() == 2024);
    CHECK(meta.trackNumber() == 1);
    CHECK(meta.trackTotal() == 10);
    CHECK(meta.genre() == "Genre");
    CHECK(meta.work() == "WorkName");
    CHECK(meta.movement() == "MovementName");
    CHECK(meta.ensemble() == "Ensemble");
    CHECK(meta.soloist() == "Soloist");
    CHECK(meta.movementNumber() == 2);
    CHECK(meta.movementTotal() == 4);

    CHECK(builder.customMetadata().pairs().empty());

    auto const& covers = builder.coverArt().entries();
    REQUIRE(covers.size() == 2);
    CHECK(covers[0].type == library::PictureType::FrontCover);
    auto const firstData = std::get<std::span<std::byte const>>(covers[0].source);
    REQUIRE_FALSE(firstData.empty());
    CHECK(firstData.size() == 2);
    CHECK(static_cast<std::uint8_t>(firstData[0]) == 0xAA);
    CHECK(covers[1].type == library::PictureType::BackCover);
    auto const secondData = std::get<std::span<std::byte const>>(covers[1].source);
    CHECK(static_cast<std::uint8_t>(secondData[0]) == 0xCC);

    auto const prop = builder.property();
    CHECK(prop.codec() == AudioCodec::Mp3);
    CHECK(prop.bitDepth() == 16);
  }

  TEST_CASE("MPEG File - maps orchestra fallback when ensemble is absent", "[tag][unit][mpeg][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("classical_fallback.mp3")};
    auto const builder = loadTrack(file);

    CHECK(builder.metadata().title() == "Classical Fallback");
    CHECK(builder.metadata().ensemble() == "Fixture Fallback Ensemble");
  }

  TEST_CASE("MPEG File - maps real fixture tags into TrackBuilder", "[tag][unit][mpeg][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.mp3")};
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

  TEST_CASE("MPEG File - audio payload range trims leading and trailing tags", "[tag][unit][mpeg][file]")
  {
    auto data = wrapId3v2(3, {});
    std::size_t const expectedOffset = data.size();
    auto const frame = createValidMpegFrame();
    data.insert(data.end(), frame.begin(), frame.end());
    appendApeV2Footer(data);
    appendId3v1Tag(data);

    auto const temp = TempFile{data, ".mp3"};
    auto const file = File{temp.path};
    auto rangeResult = file.audioPayload();

    REQUIRE(rangeResult);
    auto const range = *rangeResult;
    CHECK(range.offset == expectedOffset);
    REQUIRE(range.bytes.size() == frame.size());
    CHECK(std::to_integer<std::uint8_t>(range.bytes[0]) == frame[0]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[1]) == frame[1]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[2]) == frame[2]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[3]) == frame[3]);
  }

  TEST_CASE("MPEG File - audio payload range skips junk between ID3v2 and first frame", "[tag][unit][mpeg][file]")
  {
    auto data = wrapId3v2(3, {});
    data.insert(data.end(), {0x00, 0x11, 0x22, 0x33, 0x7F});
    std::size_t const expectedOffset = data.size();
    auto const frame = createValidMpegFrame();
    data.insert(data.end(), frame.begin(), frame.end());
    appendApeV2HeaderAndFooter(data);
    appendId3v1Tag(data);

    auto const temp = TempFile{data, ".mp3"};
    auto const file = File{temp.path};
    auto rangeResult = file.audioPayload();

    REQUIRE(rangeResult);
    auto const range = *rangeResult;
    CHECK(range.offset == expectedOffset);
    REQUIRE(range.bytes.size() == frame.size());
    CHECK(std::to_integer<std::uint8_t>(range.bytes[0]) == frame[0]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[1]) == frame[1]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[2]) == frame[2]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[3]) == frame[3]);
  }

  TEST_CASE("MPEG File - audio payload range trims APEv2 header advertised by footer", "[tag][unit][mpeg][file]")
  {
    auto data = std::vector<std::uint8_t>{};
    auto const frame = createValidMpegFrame();
    data.insert(data.end(), frame.begin(), frame.end());
    appendApeV2HeaderAndFooter(data);

    auto const temp = TempFile{data, ".mp3"};
    auto const file = File{temp.path};
    auto rangeResult = file.audioPayload();

    REQUIRE(rangeResult);
    auto const range = *rangeResult;
    CHECK(range.offset == 0);
    REQUIRE(range.bytes.size() == frame.size());
    CHECK(std::to_integer<std::uint8_t>(range.bytes[0]) == frame[0]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[1]) == frame[1]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[2]) == frame[2]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[3]) == frame[3]);
  }

  TEST_CASE("MPEG File - audio payload signature ignores tag changes", "[tag][unit][mpeg][file]")
  {
    auto firstBody = std::vector<std::uint8_t>{};
    addTextFrame(firstBody, "TIT2", "Before");
    auto secondBody = std::vector<std::uint8_t>{};
    addTextFrame(secondBody, "TIT2", "After Retag");

    auto firstData = wrapId3v2(3, firstBody);
    auto secondData = wrapId3v2(3, secondBody);
    auto const frame = createValidMpegFrame();
    firstData.insert(firstData.end(), frame.begin(), frame.end());
    secondData.insert(secondData.end(), frame.begin(), frame.end());
    appendId3v1Tag(firstData);
    appendId3v1Tag(secondData);

    auto const firstTemp = TempFile{firstData, ".mp3"};
    auto const secondTemp = TempFile{secondData, ".mp3"};
    auto const firstFile = File{firstTemp.path};
    auto const secondFile = File{secondTemp.path};

    auto const firstPayloadResult = firstFile.audioPayload();
    auto const secondPayloadResult = secondFile.audioPayload();
    REQUIRE(firstPayloadResult);
    REQUIRE(secondPayloadResult);

    CHECK(firstPayloadResult->bytes.size() == frame.size());
    CHECK(secondPayloadResult->bytes.size() == frame.size());
    CHECK(utility::xxh3Hash128(firstPayloadResult->bytes) == utility::xxh3Hash128(secondPayloadResult->bytes));
  }

  TEST_CASE("MPEG File - derives CBR audio properties", "[tag][unit][mpeg][file]")
  {
    auto data = std::vector<std::uint8_t>{};
    auto const mpegHdr = std::to_array<std::uint8_t>({0xFF, 0xFB, 0x90, 0x44});
    data.insert(data.end(), mpegHdr.begin(), mpegHdr.end());
    data.resize(data.size() + 16000, 0);

    auto const temp = TempFile{data};
    auto const file = File{temp.path};
    auto builder = loadTrack(file);

    CHECK(builder.property().duration() >= std::chrono::seconds{1});
    CHECK(builder.property().duration() <= std::chrono::milliseconds{1010});
    CHECK(builder.property().codec() == AudioCodec::Mp3);
    CHECK(builder.property().bitDepth() == 16);
  }

  TEST_CASE("MPEG File - decodes ID3v2.4 syncsafe frame sizes", "[tag][unit][mpeg][id3v2]")
  {
    // A content size >= 128 makes the v2.4 syncsafe encoding differ from a plain
    // big-endian 32-bit decode, so the parser must use the syncsafe path to step
    // over the first frame and reach the second.
    auto const longTitle = std::string(200, 'A');

    auto body = std::vector<std::uint8_t>{};
    addV24TextFrame(body, "TIT2", id3v2::Encoding::Latin1, asBytes(longTitle));
    addV24TextFrame(body, "TPE1", id3v2::Encoding::Latin1, asBytes("Artist"));

    auto const data = wrapId3v2(4, body);
    auto const temp = TempFile{data};
    auto const file = File{temp.path};
    auto const builder = loadTrack(file);
    auto const meta = builder.metadata();

    CHECK(meta.title() == longTitle);
    CHECK(meta.artist() == "Artist");
  }

  TEST_CASE("MPEG File - decodes ID3v2.4 UTF-8 and UTF-16BE text", "[tag][unit][mpeg][id3v2]")
  {
    SECTION("UTF-8")
    {
      auto const utf8 = std::to_array<std::uint8_t>({0xC3, 0xA9, 'x'}); // "éx"
      auto body = std::vector<std::uint8_t>{};
      addV24TextFrame(body, "TIT2", id3v2::Encoding::Utf8, utf8);

      auto const data = wrapId3v2(4, body);
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto const builder = loadTrack(file);
      CHECK(builder.metadata().title() == "\xC3\xA9x");
    }

    SECTION("UTF-16BE without BOM")
    {
      auto const utf16 = std::to_array<std::uint8_t>({0x00, 'H', 0x00, 'i'});
      auto body = std::vector<std::uint8_t>{};
      addV24TextFrame(body, "TIT2", id3v2::Encoding::Utf16Be, utf16);

      auto const data = wrapId3v2(4, body);
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto const builder = loadTrack(file);
      CHECK(builder.metadata().title() == "Hi");
    }
  }

  TEST_CASE("MPEG File - handles unsupported or malformed input", "[tag][unit][mpeg][file]")
  {
    SECTION("Unsupported ID3v2.2 tag")
    {
      auto data = std::vector<std::uint8_t>{};
      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", 3);
      header.majorVersion = 2;
      auto const* hdrAddr = reinterpret_cast<std::uint8_t const*>(&header);
      data.insert(data.end(), hdrAddr, hdrAddr + sizeof(header));
      data.resize(data.size() + 100, 0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto builder = loadTrack(file);
      CHECK(builder.metadata().title().empty());
    }

    SECTION("Truncated ID3v2 header")
    {
      auto data = std::vector<std::uint8_t>{};
      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", 3);
      header.majorVersion = 3;
      header.size.data[3] = 100;
      auto const* hdrAddr = reinterpret_cast<std::uint8_t const*>(&header);
      data.insert(data.end(), hdrAddr, hdrAddr + sizeof(header));
      // Give it only 10 bytes instead of 100
      data.resize(data.size() + 10, 0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto builder = loadTrack(file);
      CHECK(builder.metadata().title().empty());
    }

    SECTION("Frame size exceeds tag body")
    {
      auto data = std::vector<std::uint8_t>{};
      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", 3);
      header.majorVersion = 3;
      header.size.data[3] = 20;
      auto const* hdrAddr = reinterpret_cast<std::uint8_t const*>(&header);
      data.insert(data.end(), hdrAddr, hdrAddr + sizeof(header));

      auto titleFrame = id3v2::V23TextFrameLayout{};
      std::memcpy(titleFrame.common.id.data(), "TIT2", 4);
      titleFrame.common.size = 500; // Claims 500 bytes, but body is only 20
      titleFrame.encoding = id3v2::Encoding::Latin1;
      auto const* frameAddr = reinterpret_cast<std::uint8_t const*>(&titleFrame);
      data.insert(data.end(), frameAddr, frameAddr + sizeof(titleFrame));
      data.resize(sizeof(header) + 20, 0); // Fill to exactly 20 bytes body

      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto result = file.loadTrack();
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Missing MPEG frame sync")
    {
      auto data = std::vector<std::uint8_t>(1000, 0x42); // Just 1000 bytes of garbage, no 0xFF 0xFB sync
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto builder = loadTrack(file);
      CHECK(builder.property().duration() == std::chrono::milliseconds{0});
    }

    SECTION("Truncated APIC frame does not overrun the buffer")
    {
      // APIC body with an unterminated MIME string and no picture-type/description/
      // image data. The handler must stop at the frame boundary rather than walking
      // past it (release builds strip the gsl bounds checks).
      auto frameBody = std::vector<std::uint8_t>{0x00, 'i', 'm', 'g'};
      auto header = id3v2::V23CommonFrameLayout{};
      std::memcpy(header.id.data(), "APIC", 4);
      header.size = static_cast<std::uint32_t>(frameBody.size());

      auto body = std::vector<std::uint8_t>{};
      auto const* hdr = reinterpret_cast<std::uint8_t const*>(&header);
      body.insert(body.end(), hdr, hdr + sizeof(header));
      body.insert(body.end(), frameBody.begin(), frameBody.end());

      auto const data = wrapId3v2(3, body);
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto const builder = loadTrack(file);
      CHECK(builder.coverArt().entries().empty());
    }
  }
} // namespace ao::tag::mpeg::test
