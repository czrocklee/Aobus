// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/flac/File.h"
#include "test/unit/TestUtils.h"
#include <ao/Exception.h>
#include <ao/library/AudioCodec.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/tag/TagFile.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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

    std::vector<std::uint8_t> createMinimalFlac()
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
      std::uint32_t const count = 16;
      vc.push_back(count & 0xFF);
      vc.push_back((count >> 8) & 0xFF);
      vc.push_back((count >> 16) & 0xFF);
      vc.push_back((count >> 24) & 0xFF);
      addString("TITLE=Title");
      addString("ARTIST=Artist");
      addString("ALBUMARTIST=AlbumArtist");
      addString("COMPOSER=Composer");
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
      addString("UNKNOWN=IgnoredValue");

      addBlockHeader(data, MetadataBlockType::VorbisComment, true, static_cast<std::uint32_t>(vc.size()));
      data.insert(data.end(), vc.begin(), vc.end());

      return data;
    }
  }

  TEST_CASE("FLAC File - parses metadata and audio properties", "[tag][unit][flac][file]")
  {
    auto const data = createMinimalFlac();
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    auto const meta = builder.metadata();
    CHECK(meta.title() == "Title");
    CHECK(meta.artist() == "Artist");
    CHECK(meta.albumArtist() == "AlbumArtist");
    CHECK(meta.composer() == "Composer");
    CHECK(meta.genre() == "Genre");
    CHECK(meta.trackNumber() == 1);
    CHECK(meta.trackTotal() == 10);
    CHECK(meta.discNumber() == 2);
    CHECK(meta.discTotal() == 5);
    CHECK(meta.year() == 2024);
    CHECK(meta.work() == "WorkName");
    CHECK(meta.movement() == "MovementName");
    CHECK(meta.movementNumber() == 2);
    CHECK(meta.movementTotal() == 4);
    CHECK(builder.customMetadata().pairs().empty());

    auto const prop = builder.property();
    CHECK(prop.sampleRate() == 44100);
    CHECK(prop.channels() == 2);
    CHECK(prop.bitDepth() == 16);
    CHECK(prop.codec() == library::AudioCodec::Flac);
    CHECK(prop.duration() == std::chrono::seconds{1});
  }

  TEST_CASE("FLAC File - rejects malformed input", "[tag][unit][flac][file]")
  {
    SECTION("Missing StreamInfo block")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};
      addBlockHeader(data, MetadataBlockType::VorbisComment, true, 10);
      data.insert(data.end(), 10, 0);

      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      REQUIRE_THROWS_AS(file.loadTrack(), ao::Exception);
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      REQUIRE_THROWS_AS(file.loadTrack(), ao::Exception);
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
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      REQUIRE_THROWS_AS(file.loadTrack(), ao::Exception);
    }

    SECTION("Invalid FLAC magic signature")
    {
      auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'K'};
      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      REQUIRE_THROWS_AS(file.loadTrack(), ao::Exception);
    }
  }
} // namespace ao::tag::flac::test
