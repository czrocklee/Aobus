// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/mpeg/File.h"
#include "lib/tag/mpeg/FrameLayout.h"
#include "lib/tag/mpeg/id3v2/Layout.h"
#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/library/AudioCodec.h>
#include <ao/tag/TagFile.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
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

    std::vector<std::uint8_t> createMp3WithTags()
    {
      auto body = std::vector<std::uint8_t>{};
      addTextFrame(body, "TIT2", "Title");
      addTextFrame(body, "TPE1", "Artist");
      addTextFrame(body, "TALB", "Album");
      addTextFrame(body, "TYER", "2024");
      addTextFrame(body, "TRCK", "1/10");
      addTextFrame(body, "TCON", "Genre");

      addTxxxFrame(body, "work", "WorkName");
      addTxxxFrame(body, "CustomKey", "CustomValue");

      // APIC frame (Cover Art)
      auto apicBody = std::vector<std::uint8_t>{};
      apicBody.push_back(0); // Latin1
      apicBody.push_back('i');
      apicBody.push_back('m');
      apicBody.push_back('g');
      apicBody.push_back('/');
      apicBody.push_back('j');
      apicBody.push_back('p');
      apicBody.push_back('e');
      apicBody.push_back('g');
      apicBody.push_back(0); // Null terminator for mime type
      apicBody.push_back(3); // Front cover
      apicBody.push_back(0); // Null terminator for description
      apicBody.push_back(0xAA);
      apicBody.push_back(0xBB); // Fake image data

      auto apicHdr = id3v2::V23CommonFrameLayout{};
      std::memcpy(apicHdr.id.data(), "APIC", 4);
      apicHdr.size = static_cast<std::uint32_t>(apicBody.size());
      body.insert(body.end(),
                  reinterpret_cast<std::uint8_t const*>(&apicHdr),
                  reinterpret_cast<std::uint8_t const*>(&apicHdr) + 10);
      body.insert(body.end(), apicBody.begin(), apicBody.end());

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
  }

  TEST_CASE("MPEG File - loadTrack with tags", "[tag][unit][mpeg][file]")
  {
    auto const data = createMp3WithTags();
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    auto const meta = builder.metadata();
    CHECK(meta.title() == "Title");
    CHECK(meta.artist() == "Artist");
    CHECK(meta.album() == "Album");
    CHECK(meta.year() == 2024);
    CHECK(meta.trackNumber() == 1);
    CHECK(meta.totalTracks() == 10);
    CHECK(meta.genre() == "Genre");
    CHECK(meta.work() == "WorkName");

    CHECK(builder.custom().pairs().empty());

    REQUIRE_FALSE(meta.coverArtData().empty());
    CHECK(meta.coverArtData().size() == 2);
    CHECK(static_cast<std::uint8_t>(meta.coverArtData()[0]) == 0xAA);

    auto const prop = builder.property();
    CHECK(prop.codec() == library::AudioCodec::Mp3);
    CHECK(prop.bitDepth() == 16);
  }

  TEST_CASE("MPEG File - CBR Duration", "[tag][unit][mpeg][file]")
  {
    auto data = std::vector<std::uint8_t>{};
    auto const mpegHdr = std::array<std::uint8_t, 4>{0xFF, 0xFB, 0x90, 0x44};
    data.insert(data.end(), mpegHdr.begin(), mpegHdr.end());
    data.resize(data.size() + 16000, 0);

    auto const temp = TempFile{data};
    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    REQUIRE(builder.property().duration() >= std::chrono::seconds{1});
    CHECK(builder.property().duration() <= std::chrono::milliseconds{1010});
    CHECK(builder.property().codec() == library::AudioCodec::Mp3);
    CHECK(builder.property().bitDepth() == 16);
  }

  TEST_CASE("MPEG File - ID3v2.2 (Unsupported)", "[tag][unit][mpeg][file]")
  {
    auto data = std::vector<std::uint8_t>{};
    auto header = id3v2::HeaderLayout{};
    std::memcpy(header.id.data(), "ID3", 3);
    header.majorVersion = 2;
    auto const* hdrAddr = reinterpret_cast<std::uint8_t const*>(&header);
    data.insert(data.end(), hdrAddr, hdrAddr + sizeof(header));
    data.resize(data.size() + 100, 0);

    auto const temp = TempFile{data};
    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();
    CHECK(builder.metadata().title().empty());
  }

  TEST_CASE("MPEG File - Malformed Data", "[tag][unit][mpeg][file]")
  {
    SECTION("Truncated ID3v2 Header")
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
      CHECK(builder.metadata().title().empty());
    }

    SECTION("Corrupt Frame Size Exceeds Body")
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      REQUIRE_THROWS_AS(file.loadTrack(), ao::Exception);
    }

    SECTION("Invalid Sync")
    {
      auto data = std::vector<std::uint8_t>(1000, 0x42); // Just 1000 bytes of garbage, no 0xFF 0xFB sync
      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
      CHECK(builder.property().duration() == std::chrono::milliseconds{0});
    }
  }
} // namespace ao::tag::mpeg::test
