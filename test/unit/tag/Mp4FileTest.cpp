// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/tag/mp4/File.h"
#include "test/unit/TestUtils.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/AudioCodec.h>
#include <ao/library/CoverArt.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/mp4/AtomLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::tag::mp4::test
{
  using namespace ao::media::mp4;
  using namespace ao::test;

  namespace
  {
    std::vector<std::uint8_t> makeIntegerMetadataAtom(char const* atomType,
                                                      std::span<std::uint8_t const> payload,
                                                      std::uint32_t dataType = 21,
                                                      std::uint8_t version = 0)
    {
      auto layout = DataAtomLayout{};
      layout.common.length = static_cast<std::uint32_t>(sizeof(DataAtomLayout) + payload.size());
      std::memcpy(layout.common.type.data(), atomType, 4);
      layout.dataLength = static_cast<std::uint32_t>(16 + payload.size());
      std::memcpy(layout.magic.data(), "data", 4);
      layout.type = (static_cast<std::uint32_t>(version) << 24U) | dataType;

      auto const* ptr = reinterpret_cast<std::uint8_t const*>(&layout);
      auto atom = std::vector(ptr, ptr + sizeof(layout));
      atom.insert(atom.end(), payload.begin(), payload.end());
      return atom;
    }

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
      addTextAtom("\xA9"
                  "mvn",
                  "MovementName");

      auto const movementNumberAtom = makeIntegerMetadataAtom("\xA9"
                                                              "mvi",
                                                              std::to_array<std::uint8_t>({0, 2}));
      ilstBody.insert(ilstBody.end(), movementNumberAtom.begin(), movementNumberAtom.end());
      auto const movementTotalAtom = makeIntegerMetadataAtom("\xA9"
                                                             "mvc",
                                                             std::to_array<std::uint8_t>({0, 4}));
      ilstBody.insert(ilstBody.end(), movementTotalAtom.begin(), movementTotalAtom.end());
      addTextAtom("aART", "AlbumArtist");

      auto addCoverAtom = [&](std::uint8_t firstByte)
      {
        // Build a covr atom containing a single child data atom.
        // Child data atom layout: [length(4)]["data"(4)][type_indicator(4)][locale(4)][payload]
        auto childData = std::vector<std::uint8_t>{};
        auto childPayload = std::vector<std::uint8_t>{firstByte, static_cast<std::uint8_t>(firstByte + 1)};
        constexpr std::uint32_t kDataChildHeaderSize = 16; // length + "data" + type + locale
        auto childLength = static_cast<std::uint32_t>(kDataChildHeaderSize + childPayload.size());
        ao::test::mp4::appendBe32(childData, childLength);
        childData.push_back('d');
        childData.push_back('a');
        childData.push_back('t');
        childData.push_back('a');
        ao::test::mp4::appendBe32(childData, 13); // type indicator: JPEG
        ao::test::mp4::appendBe32(childData, 0);  // locale
        childData.insert(childData.end(), childPayload.begin(), childPayload.end());

        auto atom = std::vector<std::uint8_t>{};
        ao::test::mp4::addAtom(atom, "covr", childData);
        ilstBody.insert(ilstBody.end(), atom.begin(), atom.end());
      };
      addCoverAtom(0xCC);
      addCoverAtom(0xEE);

      // trkn atom (Track)
      {
        auto trkn = TrknAtomLayout{};
        trkn.common.common.length = sizeof(TrknAtomLayout);
        std::memcpy(trkn.common.common.type.data(), "trkn", 4);
        trkn.common.dataLength = 16 + 8; // data header (8) + fields (8)
        std::memcpy(trkn.common.magic.data(), "data", 4);
        trkn.trackNumber = 7;
        trkn.trackTotal = 10;

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
        disk.discTotal = 5;

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

    enum class MovementField : std::uint8_t
    {
      Number,
      Total,
    };

    library::TrackBuilder loadTrack(File const& file)
    {
      auto result = file.loadTrack();
      REQUIRE(result);
      return *result;
    }

    std::uint16_t loadMovementValue(MovementField field,
                                    std::span<std::uint8_t const> payload,
                                    std::uint32_t dataType = 21,
                                    std::uint8_t version = 0,
                                    std::optional<std::uint8_t> optBaseline = std::nullopt)
    {
      auto const* atomType = field == MovementField::Number ? "\xA9"
                                                              "mvi"
                                                            : "\xA9"
                                                              "mvc";
      auto ilstChildren = std::vector<std::uint8_t>{};

      if (optBaseline)
      {
        ilstChildren = makeIntegerMetadataAtom(atomType, std::array{*optBaseline});
      }

      auto const candidate = makeIntegerMetadataAtom(atomType, payload, dataType, version);
      ilstChildren.insert(ilstChildren.end(), candidate.begin(), candidate.end());

      auto const data = createMinimalM4aWithRawIlstAtom(ilstChildren);
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto const metadata = loadTrack(file).metadata();
      return field == MovementField::Number ? metadata.movementNumber() : metadata.movementTotal();
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
  } // namespace

  TEST_CASE("MP4 File - parses a complete tagged file", "[tag][unit][mp4][file]")
  {
    auto const data = createMinimalM4a();
    auto const temp = TempFile{data};

    auto const file = File{temp.path};
    auto builder = loadTrack(file);

    auto const meta = builder.metadata();
    CHECK(meta.title() == "Title");
    CHECK(meta.artist() == "Artist");
    CHECK(meta.albumArtist() == "AlbumArtist");
    CHECK(meta.album() == "Album");
    CHECK(meta.year() == 2024);
    CHECK(meta.genre() == "Genre");
    CHECK(meta.composer() == "Composer");
    CHECK(meta.work() == "Grouping"); // grp overwrites wrk
    CHECK(meta.movement() == "MovementName");
    CHECK(meta.movementNumber() == 2);
    CHECK(meta.movementTotal() == 4);
    CHECK(meta.trackNumber() == 7);
    CHECK(meta.discNumber() == 2);
    CHECK(meta.discTotal() == 5);

    auto const& covers = builder.coverArt().entries();
    REQUIRE(covers.size() == 2);
    CHECK(covers[0].type == library::PictureType::FrontCover);
    auto const firstData = std::get<std::span<std::byte const>>(covers[0].source);
    REQUIRE_FALSE(firstData.empty());
    CHECK(firstData.size() == 2);
    CHECK(static_cast<std::uint8_t>(firstData[0]) == 0xCC);
    CHECK(covers[1].type == library::PictureType::FrontCover);
    auto const secondData = std::get<std::span<std::byte const>>(covers[1].source);
    CHECK(static_cast<std::uint8_t>(secondData[0]) == 0xEE);

    CHECK(meta.discTotal() == 5);

    CHECK(builder.customMetadata().pairs().empty());

    CHECK(builder.property().sampleRate() == 44100);
    CHECK(builder.property().duration() == std::chrono::seconds{1});
    CHECK(builder.property().channels() == 2);
    CHECK(builder.property().bitDepth() == 16);
    CHECK(builder.property().codec() == AudioCodec::Aac);
  }

  TEST_CASE("MP4 File - single covr with two data boxes", "[tag][unit][mp4][file][cover]")
  {
    // Standard iTunes encoding: one covr atom containing two data children.
    // Each data child has: [length(4)]["data"(4)][type_indicator(4)][locale(4)][payload]
    auto makeDataChild = [](std::vector<std::uint8_t> const& payload)
    {
      auto child = std::vector<std::uint8_t>{};
      constexpr std::uint32_t kDataChildHeaderSize = 16;
      ao::test::mp4::appendBe32(child, kDataChildHeaderSize + static_cast<std::uint32_t>(payload.size()));
      child.push_back('d');
      child.push_back('a');
      child.push_back('t');
      child.push_back('a');
      ao::test::mp4::appendBe32(child, 13); // JPEG
      ao::test::mp4::appendBe32(child, 0);  // locale
      child.insert(child.end(), payload.begin(), payload.end());
      return child;
    };

    auto const child1 = makeDataChild({0xAA, 0xBB});
    auto const child2 = makeDataChild({0xDD, 0xEE, 0xFF});

    // Combine into a single covr atom body
    auto covrBody = std::vector<std::uint8_t>{};
    covrBody.insert(covrBody.end(), child1.begin(), child1.end());
    covrBody.insert(covrBody.end(), child2.begin(), child2.end());

    auto ilstBody = std::vector<std::uint8_t>{};
    ao::test::mp4::addAtom(ilstBody, "covr", covrBody);

    auto const ilstAtom = ao::test::mp4::makeAtom("ilst", ilstBody);
    auto metaBody = std::vector<std::uint8_t>{0, 0, 0, 0};
    metaBody.insert(metaBody.end(), ilstAtom.begin(), ilstAtom.end());
    auto const metaAtom = ao::test::mp4::makeAtom("meta", metaBody);
    auto udtaBody = std::vector<std::uint8_t>{};
    udtaBody.insert(udtaBody.end(), metaAtom.begin(), metaAtom.end());
    auto const udtaAtom = ao::test::mp4::makeAtom("udta", udtaBody);
    auto const trakAtom = ao::test::mp4::makeAudioTrackAtom("mp4a");

    auto data = std::vector<std::uint8_t>{};
    auto moovBody = std::vector<std::uint8_t>{};
    moovBody.insert(moovBody.end(), udtaAtom.begin(), udtaAtom.end());
    moovBody.insert(moovBody.end(), trakAtom.begin(), trakAtom.end());
    ao::test::mp4::addAtom(data, "moov", moovBody);

    auto const temp = TempFile{data};
    auto const file = File{temp.path};
    auto builder = loadTrack(file);

    auto const& covers = builder.coverArt().entries();
    REQUIRE(covers.size() == 2);

    CHECK(covers[0].type == library::PictureType::FrontCover);
    auto const firstData = std::get<std::span<std::byte const>>(covers[0].source);
    REQUIRE(firstData.size() == 2);
    CHECK(static_cast<std::uint8_t>(firstData[0]) == 0xAA);
    CHECK(static_cast<std::uint8_t>(firstData[1]) == 0xBB);

    CHECK(covers[1].type == library::PictureType::FrontCover);
    auto const secondData = std::get<std::span<std::byte const>>(covers[1].source);
    REQUIRE(secondData.size() == 3);
    CHECK(static_cast<std::uint8_t>(secondData[0]) == 0xDD);
    CHECK(static_cast<std::uint8_t>(secondData[1]) == 0xEE);
    CHECK(static_cast<std::uint8_t>(secondData[2]) == 0xFF);
  }

  TEST_CASE("MP4 File - parses movement integer metadata", "[tag][unit][mp4][file]")
  {
    SECTION("Accepts every supported payload width")
    {
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 1>{0x7F}) == 127);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 2>{0x01, 0x02}) == 258);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 3>{0x00, 0xFF, 0xFF}) == 65535);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 4>{0x00, 0x00, 0x12, 0x34}) == 0x1234);
      CHECK(loadMovementValue(MovementField::Number,
                              std::array<std::uint8_t, 8>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x45}) == 0x2345);
    }

    SECTION("Accepts the implicit integer data type")
    {
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 2>{0x01, 0x23}, 0) == 0x0123);
    }

    SECTION("Rejects unsupported payload widths")
    {
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 0>{}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 5>{0, 0, 0, 0, 1}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 6>{0, 0, 0, 0, 0, 1}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 7>{0, 0, 0, 0, 0, 0, 1}, 21, 0, 37) ==
            37);
      CHECK(loadMovementValue(
              MovementField::Number, std::array<std::uint8_t, 9>{0, 0, 0, 0, 0, 0, 0, 0, 1}, 21, 0, 37) == 37);
    }

    SECTION("Rejects negative values")
    {
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 1>{0xFF}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 2>{0x80, 0x00}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 3>{0xFF, 0xFF, 0xFF}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 4>{0x80, 0x00, 0x00, 0x00}, 21, 0, 37) ==
            37);
      CHECK(loadMovementValue(MovementField::Number,
                              std::array<std::uint8_t, 8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                              21,
                              0,
                              37) == 37);
    }

    SECTION("Rejects values outside uint16 range")
    {
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 3>{0x01, 0x00, 0x00}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 4>{0x7F, 0xFF, 0xFF, 0xFF}, 21, 0, 37) ==
            37);
      CHECK(loadMovementValue(MovementField::Number,
                              std::array<std::uint8_t, 8>{0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00},
                              21,
                              0,
                              37) == 37);
    }

    SECTION("Rejects incompatible data type and version")
    {
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 1>{42}, 1, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 1>{42}, 13, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Number, std::array<std::uint8_t, 1>{42}, 21, 1, 37) == 37);
    }

    SECTION("Applies the same validation to movement total")
    {
      CHECK(loadMovementValue(MovementField::Total,
                              std::array<std::uint8_t, 8>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x45},
                              21,
                              0,
                              37) == 0x2345);
      CHECK(loadMovementValue(MovementField::Total, std::array<std::uint8_t, 5>{0, 0, 0, 0, 1}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Total, std::array<std::uint8_t, 1>{0xFF}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Total, std::array<std::uint8_t, 3>{0x01, 0x00, 0x00}, 21, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Total, std::array<std::uint8_t, 1>{42}, 13, 0, 37) == 37);
      CHECK(loadMovementValue(MovementField::Total, std::array<std::uint8_t, 1>{42}, 21, 1, 37) == 37);
    }
  }

  TEST_CASE("MP4 File - parses track and disc number pairs", "[tag][unit][mp4][file]")
  {
    SECTION("Accepts a 32-byte disk atom")
    {
      auto const data = createMinimalM4aWithRawDiskAtom();
      auto const temp = TempFile{data};

      auto const file = File{temp.path};
      auto builder = loadTrack(file);

      CHECK(builder.metadata().discNumber() == 2);
      CHECK(builder.metadata().discTotal() == 5);
    }

    SECTION("Accepts a 30-byte trkn atom")
    {
      auto const data = createMinimalM4aWithRawTrackAtom();
      auto const temp = TempFile{data};

      auto const file = File{temp.path};
      auto builder = loadTrack(file);

      CHECK(builder.metadata().trackNumber() == 2);
      CHECK(builder.metadata().trackTotal() == 4);
    }
  }

  TEST_CASE("MP4 File - derives audio properties", "[tag][unit][mp4][file]")
  {
    SECTION("Recognizes ALAC sample entries")
    {
      auto const data = createMinimalM4a("alac");
      auto const temp = TempFile{data};

      auto const file = File{temp.path};
      auto builder = loadTrack(file);

      CHECK(builder.property().codec() == AudioCodec::Alac);
    }

    SECTION("Reads AAC entries that contain child atoms")
    {
      auto const esdsAtom = ao::test::mp4::makeAtom("esds", {0, 0, 0, 0});
      auto const data = createMinimalM4a("mp4a", esdsAtom);
      auto const temp = TempFile{data};

      auto const file = File{temp.path};
      auto builder = loadTrack(file);

      CHECK(builder.property().codec() == AudioCodec::Aac);
      CHECK(builder.property().sampleRate() == 44100);
      CHECK(builder.property().channels() == 2);
      CHECK(builder.property().bitDepth() == 16);
    }

    SECTION("Skips a leading video track")
    {
      auto const data = createMinimalM4aWithLeadingVideoTrack();
      auto const temp = TempFile{data};

      auto const file = File{temp.path};
      auto builder = loadTrack(file);

      CHECK(builder.property().codec() == AudioCodec::Aac);
      CHECK(builder.property().sampleRate() == 48000);
      CHECK(builder.property().duration().count() == 2000);
      CHECK(builder.property().channels() == 2);
      CHECK(builder.property().bitDepth() == 16);
    }
  }

  TEST_CASE("MP4 File - handles malformed input", "[tag][unit][mp4][file]")
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
      auto const file = File{temp.path};
      // The parseAtoms loop checks (data.size() < kAtomHeaderSize) and (length > data.size()),
      // so it should just gracefully skip the rest or stop parsing without crashing.
      auto builder = loadTrack(file);
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
      auto const file = File{temp.path};
      auto builder = loadTrack(file);
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
      auto const file = File{temp.path};
      auto builder = loadTrack(file);
      CHECK(builder.metadata().title().empty());
    }

    SECTION("Metadata atom shorter than the data-atom header")
    {
      // A ©nam atom whose declared length (16) is smaller than the fixed data-atom
      // header (24). atomData() must refuse to interpret it instead of underflowing
      // the payload size and reading past the atom.
      auto child = std::vector<std::uint8_t>{};
      auto common = AtomLayout{};
      common.length = 16;
      std::memcpy(common.type.data(), "\xA9nam", 4);
      auto const* c = reinterpret_cast<std::uint8_t const*>(&common);
      child.insert(child.end(), c, c + sizeof(common));
      child.insert(child.end(), 8, 0); // filler to reach the declared length

      auto const data = createMinimalM4aWithRawIlstAtom(child);
      auto const temp = TempFile{data};
      auto const file = File{temp.path};
      auto const builder = loadTrack(file);
      CHECK(builder.metadata().title().empty());
    }
  }
} // namespace ao::tag::mp4::test
