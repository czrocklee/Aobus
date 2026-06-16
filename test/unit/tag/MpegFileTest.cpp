// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/mpeg/File.h"
#include "lib/tag/mpeg/FrameLayout.h"
#include "lib/tag/mpeg/id3v2/Layout.h"
#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/CoverArt.h>
#include <ao/tag/TagFile.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
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

    std::vector<std::uint8_t> createMp3WithTags()
    {
      auto body = std::vector<std::uint8_t>{};
      addTextFrame(body, "TIT2", "Title");
      addTextFrame(body, "TPE1", "Artist");
      addTextFrame(body, "TALB", "Album");
      addTextFrame(body, "TYER", "2024");
      addTextFrame(body, "TRCK", "1/10");
      addTextFrame(body, "TCON", "Genre");
      addTextFrame(body, "MVNM", "MovementName");
      addTextFrame(body, "MVIN", "2/4");

      addTxxxFrame(body, "work", "WorkName");
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
  }

  TEST_CASE("MPEG File - parses metadata and cover art", "[tag][unit][mpeg][file]")
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
    CHECK(meta.trackTotal() == 10);
    CHECK(meta.genre() == "Genre");
    CHECK(meta.work() == "WorkName");
    CHECK(meta.movement() == "MovementName");
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
    CHECK(prop.codec() == library::AudioCodec::Mp3);
    CHECK(prop.bitDepth() == 16);
  }

  TEST_CASE("MPEG File - derives CBR audio properties", "[tag][unit][mpeg][file]")
  {
    auto data = std::vector<std::uint8_t>{};
    auto const mpegHdr = std::to_array<std::uint8_t>({0xFF, 0xFB, 0x90, 0x44});
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      REQUIRE_THROWS_AS(file.loadTrack(), ao::Exception);
    }

    SECTION("Missing MPEG frame sync")
    {
      auto data = std::vector<std::uint8_t>(1000, 0x42); // Just 1000 bytes of garbage, no 0xFF 0xFB sync
      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
      CHECK(builder.property().duration() == std::chrono::milliseconds{0});
    }
  }
} // namespace ao::tag::mpeg::test
