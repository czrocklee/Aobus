// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ao/media/mp4/AtomLayout.h"
#include "ao/tag/TagFile.h"
#include "lib/tag/mp4/File.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace ao::media::mp4;

namespace ao::tag::mp4::test
{
  namespace
  {
    struct TempFile final
    {
      fs::path path;
      TempFile(std::vector<std::uint8_t> const& data)
      {
        path = fs::temp_directory_path() / "ao_mp4_test_XXXXXX";
        auto ofs = std::ofstream{path, std::ios::binary};
        ofs.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
      }
      ~TempFile() { fs::remove(path); }

      TempFile(TempFile const&) = delete;
      TempFile& operator=(TempFile const&) = delete;
      TempFile(TempFile&&) = delete;
      TempFile& operator=(TempFile&&) = delete;
    };

    void addAtom(std::vector<std::uint8_t>& buffer, char const* type, std::vector<std::uint8_t> const& body)
    {
      std::uint32_t const length = 8 + static_cast<std::uint32_t>(body.size());
      auto lenBuf = boost::endian::big_uint32_buf_t{};
      lenBuf = length;
      auto const* lenPtr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      buffer.insert(buffer.end(), lenPtr, lenPtr + 4);
      buffer.push_back(type[0]);
      buffer.push_back(type[1]);
      buffer.push_back(type[2]);
      buffer.push_back(type[3]);
      buffer.insert(buffer.end(), body.begin(), body.end());
    }

    std::vector<std::uint8_t> createMinimalM4a()
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
        auto const* dlPtr = reinterpret_cast<std::uint8_t const*>(&dataLayout);
        namData.insert(namData.end(), dlPtr + 8, dlPtr + 24);
        namData.insert(namData.end(), text.begin(), text.end());
        auto atom = std::vector<std::uint8_t>{};
        addAtom(atom, type, namData);
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
        auto const* dlPtr = reinterpret_cast<std::uint8_t const*>(&dataLayout);
        covrData.insert(covrData.end(), dlPtr + 8, dlPtr + 24);
        covrData.push_back(0xCC);
        covrData.push_back(0xDD);
        auto atom = std::vector<std::uint8_t>{};
        addAtom(atom, "covr", covrData);
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
        auto atom = std::vector<std::uint8_t>(ptr, ptr + sizeof(trkn));
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
        auto atom = std::vector<std::uint8_t>(ptr, ptr + sizeof(disk));
        ilstBody.insert(ilstBody.end(), atom.begin(), atom.end());
      }

      // Unknown atom
      addTextAtom("XUNK", "UnknownValue");

      auto ilstAtom = std::vector<std::uint8_t>{};
      addAtom(ilstAtom, "ilst", ilstBody);

      auto metaBody = std::vector<std::uint8_t>{0, 0, 0, 0}; // Meta header
      metaBody.insert(metaBody.end(), ilstAtom.begin(), ilstAtom.end());

      auto metaAtom = std::vector<std::uint8_t>{};
      addAtom(metaAtom, "meta", metaBody);

      auto udtaBody = std::vector<std::uint8_t>{};
      udtaBody.insert(udtaBody.end(), metaAtom.begin(), metaAtom.end());

      auto udtaAtom = std::vector<std::uint8_t>{};
      addAtom(udtaAtom, "udta", udtaBody);

      // 2. Build trak
      auto mdhd = MdhdAtomLayout{};
      mdhd.common.length = sizeof(MdhdAtomLayout);
      std::memcpy(mdhd.common.type.data(), "mdhd", 4);
      mdhd.timescale = 44100;
      mdhd.duration = 44100; // 1 second
      auto const* mdhdPtr = reinterpret_cast<std::uint8_t const*>(&mdhd);
      auto mdhdBody = std::vector<std::uint8_t>{};
      mdhdBody.insert(mdhdBody.end(), mdhdPtr + 8, mdhdPtr + sizeof(MdhdAtomLayout));

      auto mdhdAtom = std::vector<std::uint8_t>{};
      addAtom(mdhdAtom, "mdhd", mdhdBody);

      // 3. Build stsd
      auto mp4a = AudioSampleEntryLayout{};
      mp4a.common.length = sizeof(AudioSampleEntryLayout);
      std::memcpy(mp4a.common.type.data(), "mp4a", 4);
      mp4a.channelCount = 2;
      mp4a.sampleSize = 16;
      mp4a.sampleRate = (44100 << 16);
      auto const* mp4aPtr = reinterpret_cast<std::uint8_t const*>(&mp4a);
      auto mp4aBody = std::vector<std::uint8_t>{};
      mp4aBody.insert(mp4aBody.end(), mp4aPtr + 8, mp4aPtr + sizeof(AudioSampleEntryLayout));

      auto mp4aAtom = std::vector<std::uint8_t>{};
      addAtom(mp4aAtom, "mp4a", mp4aBody);

      auto stsdBody = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1};
      stsdBody.insert(stsdBody.end(), mp4aAtom.begin(), mp4aAtom.end());

      auto stsdAtom = std::vector<std::uint8_t>{};
      addAtom(stsdAtom, "stsd", stsdBody);

      auto stblBody = std::vector<std::uint8_t>{};
      stblBody.insert(stblBody.end(), stsdAtom.begin(), stsdAtom.end());

      auto stblAtom = std::vector<std::uint8_t>{};
      addAtom(stblAtom, "stbl", stblBody);

      auto minfBody = std::vector<std::uint8_t>{};
      minfBody.insert(minfBody.end(), stblAtom.begin(), stblAtom.end());

      auto minfAtom = std::vector<std::uint8_t>{};
      addAtom(minfAtom, "minf", minfBody);

      auto mdiaBody = std::vector<std::uint8_t>{};
      mdiaBody.insert(mdiaBody.end(), mdhdAtom.begin(), mdhdAtom.end());
      mdiaBody.insert(mdiaBody.end(), minfAtom.begin(), minfAtom.end());

      auto mdiaAtom = std::vector<std::uint8_t>{};
      addAtom(mdiaAtom, "mdia", mdiaBody);

      auto trakBody = std::vector<std::uint8_t>{};
      trakBody.insert(trakBody.end(), mdiaAtom.begin(), mdiaAtom.end());

      auto trakAtom = std::vector<std::uint8_t>{};
      addAtom(trakAtom, "trak", trakBody);

      // 4. Assemble moov
      auto moovBody = std::vector<std::uint8_t>{};
      moovBody.insert(moovBody.end(), udtaAtom.begin(), udtaAtom.end());
      moovBody.insert(moovBody.end(), trakAtom.begin(), trakAtom.end());

      addAtom(data, "moov", moovBody);

      return data;
    }
  }

  TEST_CASE("MP4 File - loadTrack", "[tag][mp4][file]")
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
  }

  TEST_CASE("MP4 File - Malformed Data", "[tag][mp4][file]")
  {
    SECTION("Truncated Atom")
    {
      auto data = std::vector<std::uint8_t>{};
      // moov atom claiming to be 500 bytes long, but we only give it 8
      std::uint32_t const length = 500;
      auto lenBuf = boost::endian::big_uint32_buf_t{};
      lenBuf = length;
      auto const* lenPtr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      data.insert(data.end(), lenPtr, lenPtr + 4);
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
      auto const* lenPtr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      data.insert(data.end(), lenPtr, lenPtr + 4);
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
      auto const* lenPtr = reinterpret_cast<std::uint8_t const*>(&lenBuf);
      data.insert(data.end(), lenPtr, lenPtr + 4);
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
