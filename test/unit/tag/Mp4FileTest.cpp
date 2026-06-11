// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/mp4/File.h"
#include "test/unit/TestUtils.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/library/AudioCodec.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/tag/TagFile.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace ao::tag::mp4::test
{
  using namespace ao::media::mp4;
  using namespace ao::test;

  namespace
  {
    std::vector<std::uint8_t> createMinimalM4a(char const* sampleEntryType = "mp4a",
                                               std::vector<std::uint8_t> const& sampleEntryExtensions = {})
    {
      auto data = std::vector<std::uint8_t>{};

      // 1. Build ilst
      auto ilstBody = std::vector<std::uint8_t>{};

      auto addTextAtom = [&](char const* type, std::string_view text)
      {
        auto namData = std::vector<std::uint8_t>{};
        auto dataLayout = DataAtomLayout{};
        dataLayout.common.length = static_cast<std::uint32_t>(16 + text.size());
        std::memcpy(dataLayout.common.type.data(), "data", 4);
        dataLayout.type = 1; // Text
        auto const* dlAddr = reinterpret_cast<std::uint8_t const*>(&dataLayout);
        namData.insert(namData.end(), dlAddr + 8, dlAddr + 24);
        namData.insert(namData.end(), text.begin(), text.end());
        auto atom = std::vector<std::uint8_t>{};
        ao::test::mp4::addAtom(atom, type, namData);
        ilstBody.insert(ilstBody.end(), atom.begin(), atom.end());
      };

      addTextAtom("\xA9"
                  "nam",
                  "Title");
      addTextAtom("\xA9"
                  "ART",
                  "Artist");
      addTextAtom("\xA9"
                  "alb",
                  "Album");
      addTextAtom("\xA9"
                  "day",
                  "2024");
      addTextAtom("\xA9"
                  "gen",
                  "Genre");
      addTextAtom("\xA9"
                  "wrt",
                  "Composer");
      addTextAtom("\xA9"
                  "wrk",
                  "Work");
      addTextAtom("\xA9"
                  "grp",
                  "Grouping");
      addTextAtom("aART", "AlbumArtist");

      // covr atom
      {
        auto covrData = std::vector<std::uint8_t>{};
        auto dataLayout = DataAtomLayout{};
        dataLayout.common.length = 16 + 2;
        std::memcpy(dataLayout.common.type.data(), "data", 4);
        dataLayout.type = 13; // JPEG
        auto const* dlAddr = reinterpret_cast<std::uint8_t const*>(&dataLayout);
        covrData.insert(covrData.end(), dlAddr + 8, dlAddr + 24);
        covrData.push_back(0xCC);
        covrData.push_back(0xDD);
        auto atom = std::vector<std::uint8_t>{};
        ao::test::mp4::addAtom(atom, "covr", covrData);
        ilstBody.insert(ilstBody.end(), atom.begin(), atom.end());
      }

      // trkn atom (Track)
      {
        auto trkn = TrknAtomLayout{};
        trkn.common.common.length = sizeof(TrknAtomLayout);
        std::memcpy(trkn.common.common.type.data(), "trkn", 4);
        trkn.common.dataLength = 16 + 8; // data header (8) + fields (8)
        std::memcpy(trkn.common.magic.data(), "data", 4);
        trkn.trackNumber = 7;
        trkn.totalTracks = 10;

        auto const* ptr = reinterpret_cast<std::uint8_t const*>(&trkn);
        auto atom = std::vector(ptr, ptr + sizeof(trkn));
        ilstBody.insert(ilstBody.end(), atom.begin(), atom.end());
      }

      // disk atom (Disc)
      {
        auto disk = DiskAtomLayout{};
        disk.common.common.length = sizeof(DiskAtomLayout);
        std::memcpy(disk.common.common.type.data(), "disk", 4);
        disk.common.dataLength = 16 + 6;
        std::memcpy(disk.common.magic.data(), "data", 4);
        disk.discNumber = 2;
        disk.totalDiscs = 5;

        auto const* ptr = reinterpret_cast<std::uint8_t const*>(&disk);
        auto atom = std::vector(ptr, ptr + sizeof(disk));
        ilstBody.insert(ilstBody.end(), atom.begin(), atom.end());
      }

      // Unknown atom
      addTextAtom("XUNK", "UnknownValue");

      auto const ilstAtom = ao::test::mp4::makeAtom("ilst", ilstBody);

      auto metaBody = std::vector<std::uint8_t>{0, 0, 0, 0}; // Meta header
      metaBody.insert(metaBody.end(), ilstAtom.begin(), ilstAtom.end());

      auto const metaAtom = ao::test::mp4::makeAtom("meta", metaBody);

      auto udtaBody = std::vector<std::uint8_t>{};
      udtaBody.insert(udtaBody.end(), metaAtom.begin(), metaAtom.end());

      auto const udtaAtom = ao::test::mp4::makeAtom("udta", udtaBody);
      auto const trakAtom = ao::test::mp4::makeAudioTrackAtom(sampleEntryType, sampleEntryExtensions);

      // 4. Assemble moov
      auto moovBody = std::vector<std::uint8_t>{};
      moovBody.insert(moovBody.end(), udtaAtom.begin(), udtaAtom.end());
      moovBody.insert(moovBody.end(), trakAtom.begin(), trakAtom.end());

      ao::test::mp4::addAtom(data, "moov", moovBody);

      return data;
    }

    std::vector<std::uint8_t> createMinimalM4aWithRawIlstAtom(std::vector<std::uint8_t> const& rawIlstChild)
    {
      auto data = std::vector<std::uint8_t>{};
      auto const ilstAtom = ao::test::mp4::makeAtom("ilst", rawIlstChild);

      auto metaBody = std::vector<std::uint8_t>{0, 0, 0, 0};
      metaBody.insert(metaBody.end(), ilstAtom.begin(), ilstAtom.end());

      auto const metaAtom = ao::test::mp4::makeAtom("meta", metaBody);
      auto const udtaAtom = ao::test::mp4::makeAtom("udta", metaAtom);
      auto const trakAtom = ao::test::mp4::makeAudioTrackAtom("mp4a");

      auto moovBody = std::vector<std::uint8_t>{};
      moovBody.insert(moovBody.end(), udtaAtom.begin(), udtaAtom.end());
      moovBody.insert(moovBody.end(), trakAtom.begin(), trakAtom.end());

      ao::test::mp4::addAtom(data, "moov", moovBody);
      return data;
    }

    std::vector<std::uint8_t> createMinimalM4aWithRawDiskAtom()
    {
      auto const diskAtom = std::vector<std::uint8_t>{
        0x00, 0x00, 0x00, 0x20,                         // disk atom size: 32
        'd',  'i',  's',  'k',  0x00, 0x00, 0x00, 0x18, // data atom size: 24
        'd',  'a',  't',  'a',  0x00, 0x00, 0x00, 0x00, // type
        0x00, 0x00, 0x00, 0x00,                         // reserved
        0x00, 0x00,                                     // padding
        0x00, 0x02,                                     // disc number
        0x00, 0x05,                                     // total discs
        0x00, 0x00                                      // padding
      };

      return createMinimalM4aWithRawIlstAtom(diskAtom);
    }

    std::vector<std::uint8_t> createMinimalM4aWithRawTrackAtom()
    {
      auto const trackAtom = std::vector<std::uint8_t>{
        0x00, 0x00, 0x00, 0x1E,                         // trkn atom size: 30
        't',  'r',  'k',  'n',  0x00, 0x00, 0x00, 0x16, // data atom size: 22
        'd',  'a',  't',  'a',  0x00, 0x00, 0x00, 0x00, // type
        0x00, 0x00, 0x00, 0x00,                         // reserved
        0x00, 0x00,                                     // padding
        0x00, 0x02,                                     // track number
        0x00, 0x04                                      // total tracks
      };

      return createMinimalM4aWithRawIlstAtom(trackAtom);
    }

    std::vector<std::uint8_t> createMinimalM4aWithLeadingVideoTrack()
    {
      auto data = std::vector<std::uint8_t>{};
      auto const videoTrack = ao::test::mp4::makeVideoTrackAtom("avc1");
      auto const audioTrack = ao::test::mp4::makeCompleteAudioTrackAtom("mp4a", {}, 48000, 96000);

      auto moovBody = std::vector<std::uint8_t>{};
      moovBody.insert(moovBody.end(), videoTrack.begin(), videoTrack.end());
      moovBody.insert(moovBody.end(), audioTrack.begin(), audioTrack.end());

      ao::test::mp4::addAtom(data, "moov", moovBody);
      return data;
    }
  }

  TEST_CASE("MP4 File - loadTrack", "[tag][unit][mp4][file]")
  {
    auto const data = createMinimalM4a();
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    auto const meta = builder.metadata();
    CHECK(meta.title() == "Title");
    CHECK(meta.artist() == "Artist");
    CHECK(meta.albumArtist() == "AlbumArtist");
    CHECK(meta.album() == "Album");
    CHECK(meta.year() == 2024);
    CHECK(meta.genre() == "Genre");
    CHECK(meta.composer() == "Composer");
    CHECK(meta.work() == "Grouping"); // grp overwrites wrk
    CHECK(meta.trackNumber() == 7);
    CHECK(meta.discNumber() == 2);
    CHECK(meta.totalDiscs() == 5);

    REQUIRE_FALSE(meta.coverArtData().empty());
    CHECK(meta.coverArtData().size() == 2);
    CHECK(static_cast<std::uint8_t>(meta.coverArtData()[0]) == 0xCC);

    CHECK(meta.totalDiscs() == 5);

    // Check custom atom
    bool foundUnknown = false;

    for (auto const& [key, value] : builder.custom().pairs())
    {
      if (key == "XUNK")
      {
        CHECK(value == "UnknownValue");
        foundUnknown = true;
      }
    }

    CHECK(foundUnknown);

    CHECK(builder.property().sampleRate() == 44100);
    CHECK(builder.property().durationMs() == 1000);
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
    CHECK(builder.property().codec() == library::AudioCodec::Aac);
  }

  TEST_CASE("MP4 File - ALAC sample entry sets codec", "[tag][unit][mp4][file]")
  {
    auto const data = createMinimalM4a("alac");
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    CHECK(builder.property().codec() == library::AudioCodec::Alac);
  }

  TEST_CASE("MP4 File - 32-byte disk atom parses disc numbers", "[tag][unit][mp4][file]")
  {
    auto const data = createMinimalM4aWithRawDiskAtom();
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    CHECK(builder.metadata().discNumber() == 2);
    CHECK(builder.metadata().totalDiscs() == 5);
  }

  TEST_CASE("MP4 File - 30-byte trkn atom parses track numbers", "[tag][unit][mp4][file]")
  {
    auto const data = createMinimalM4aWithRawTrackAtom();
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    CHECK(builder.metadata().trackNumber() == 2);
    CHECK(builder.metadata().totalTracks() == 4);
  }

  TEST_CASE("MP4 File - AAC sample entry with child atoms does not crash", "[tag][unit][mp4][file]")
  {
    auto const esdsAtom = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0});
    auto const data = createMinimalM4a("mp4a", esdsAtom);
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    CHECK(builder.property().codec() == library::AudioCodec::Aac);
    CHECK(builder.property().sampleRate() == 44100);
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
  }

  TEST_CASE("MP4 File - audio properties skip leading video track", "[tag][unit][mp4][file]")
  {
    auto const data = createMinimalM4aWithLeadingVideoTrack();
    auto const temp = TempFile{data};

    auto const file = File{temp.path, TagFile::Mode::ReadOnly};
    auto builder = file.loadTrack();

    CHECK(builder.property().codec() == library::AudioCodec::Aac);
    CHECK(builder.property().sampleRate() == 48000);
    CHECK(builder.property().durationMs() == 2000);
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
  }

  TEST_CASE("MP4 File - Malformed Data", "[tag][unit][mp4][file]")
  {
    SECTION("Truncated Atom")
    {
      auto data = std::vector<std::uint8_t>{};
      // moov atom claiming to be 500 bytes long, but we only give it 8
      std::uint32_t const length = 500;
      auto lenBuf = boost::endian::big_uint32_buf_t{};
      lenBuf = length;
      auto const* lenAddr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      data.insert(data.end(), lenAddr, lenAddr + 4);
      data.push_back('m');
      data.push_back('o');
      data.push_back('o');
      data.push_back('v');

      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      // The parseAtoms loop checks (data.size() < kAtomHeaderSize) and (length > data.size()),
      // so it should just gracefully skip the rest or stop parsing without crashing.
      auto builder = file.loadTrack();
      CHECK(builder.metadata().title().empty());
    }

    SECTION("Zero-Length Trap")
    {
      auto data = std::vector<std::uint8_t>{};
      // atom claiming to be 0 bytes long (meaning extends to EOF)
      std::uint32_t const length = 0;
      auto lenBuf = boost::endian::big_uint32_buf_t{};
      lenBuf = length;
      auto const* lenAddr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      data.insert(data.end(), lenAddr, lenAddr + 4);
      data.push_back('m');
      data.push_back('o');
      data.push_back('o');
      data.push_back('v');

      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
      CHECK(builder.metadata().title().empty());
    }

    SECTION("Length less than header size")
    {
      auto data = std::vector<std::uint8_t>{};
      std::uint32_t const length = 4; // Too small for header
      auto lenBuf = boost::endian::big_uint32_buf_t{};
      lenBuf = length;
      auto const* lenAddr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      data.insert(data.end(), lenAddr, lenAddr + 4);
      data.push_back('m');
      data.push_back('o');
      data.push_back('o');
      data.push_back('v');

      auto const temp = TempFile{data};
      auto const file = File{temp.path, TagFile::Mode::ReadOnly};
      auto builder = file.loadTrack();
      CHECK(builder.metadata().title().empty());
    }
  }
} // namespace ao::tag::mp4::test
