// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/file/mpeg/id3v2/Reader.h"

#include "lib/media/file/detail/Content.h"
#include "lib/media/file/mpeg/id3v2/Frame.h"
#include "lib/media/file/mpeg/id3v2/Layout.h"
#include "test/unit/media/file/id3v2/TestId3v2.h"
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::media::file::mpeg::id3v2::test
{
  namespace
  {
    using ao::test::id3v2::appendFrame;
    using ao::test::id3v2::appendSyncSafe;
    using ao::test::id3v2::appendText;
    using ao::test::id3v2::appendUint32Be;
    using ao::test::id3v2::Bytes;
    using ao::test::id3v2::byteSpan;
    using ao::test::id3v2::deunsynchronisedSize;
    using ao::test::id3v2::makeHeader;
    using ao::test::id3v2::textContent;

    void appendByte(Bytes& out, std::uint8_t value)
    {
      out.push_back(std::byte{value});
    }

    // Latin-1 APIC body: image/jpeg mime, front cover, empty description.
    Bytes pictureContent(Bytes const& storedData)
    {
      auto content = Bytes{};
      appendByte(content, static_cast<std::uint8_t>(Encoding::Latin1));
      appendText(content, "image/jpeg");
      appendByte(content, 0); // mime terminator
      appendByte(content, 3); // front cover
      appendByte(content, 0); // description terminator
      content.insert(content.end(), storedData.begin(), storedData.end());
      return content;
    }
  } // namespace

  TEST_CASE("ID3v2 - converts text encodings to UTF-8", "[media][unit][mpeg][id3v2]")
  {
    SECTION("Latin1 to UTF-8")
    {
      auto input = std::string{"H\xE9llo"}; // Latin1 'é' is 0xE9
      auto output = convertToUtf8(utility::bytes::view(input), Encoding::Latin1);
      CHECK(output == "H\xC3\xA9llo"); // UTF-8 'é' is 0xC3 0xA9
    }

    SECTION("Latin1 ASCII remains unchanged")
    {
      auto input = std::string{"Plain ASCII"};
      CHECK(convertToUtf8(utility::bytes::view(input), Encoding::Latin1) == input);
    }

    SECTION("UCS-2 BE to UTF-8")
    {
      // BOM (FE FF) + "Test" in UTF-16BE
      auto input = std::vector{std::byte{0xFE},
                               std::byte{0xFF},
                               std::byte{0x00},
                               std::byte{'T'},
                               std::byte{0x00},
                               std::byte{'e'},
                               std::byte{0x00},
                               std::byte{'s'},
                               std::byte{0x00},
                               std::byte{'t'}};
      auto output = convertToUtf8(input, Encoding::Ucs2);
      CHECK(output == "Test");
    }

    SECTION("UCS-2 LE to UTF-8")
    {
      // BOM (FF FE) + "Test" in UTF-16LE
      auto input = std::vector{std::byte{0xFF},
                               std::byte{0xFE},
                               std::byte{'T'},
                               std::byte{0x00},
                               std::byte{'e'},
                               std::byte{0x00},
                               std::byte{'s'},
                               std::byte{0x00},
                               std::byte{'t'},
                               std::byte{0x00}};
      auto output = convertToUtf8(input, Encoding::Ucs2);
      CHECK(output == "Test");
    }

    SECTION("UCS-2 without BOM (defaults to BE)")
    {
      auto input = std::vector{std::byte{0x00}, std::byte{'T'}, std::byte{0x00}, std::byte{'e'}};
      auto output = convertToUtf8(input, Encoding::Ucs2);
      CHECK(output == "Te");
    }

    SECTION("UTF-8 passes through unchanged (ID3v2.4)")
    {
      auto input = std::string{"H\xC3\xA9llo"}; // already UTF-8 'é'
      auto output = convertToUtf8(utility::bytes::view(input), Encoding::Utf8);
      CHECK(output == "H\xC3\xA9llo");
    }

    SECTION("UTF-8 strips a leading BOM (ID3v2.4)")
    {
      auto input = std::vector{std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}, std::byte{'H'}, std::byte{'i'}};
      auto output = convertToUtf8(input, Encoding::Utf8);
      CHECK(output == "Hi");
    }

    SECTION("UTF-16BE without BOM (ID3v2.4)")
    {
      auto input = std::vector{std::byte{0x00},
                               std::byte{'T'},
                               std::byte{0x00},
                               std::byte{'e'},
                               std::byte{0x00},
                               std::byte{'s'},
                               std::byte{0x00},
                               std::byte{'t'}};
      auto output = convertToUtf8(input, Encoding::Utf16Be);
      CHECK(output == "Test");
    }

    SECTION("UTF-16BE decodes surrogate pairs (ID3v2.4)")
    {
      auto input = std::vector{std::byte{0xD8}, std::byte{0x3D}, std::byte{0xDE}, std::byte{0x00}};
      auto output = convertToUtf8(input, Encoding::Utf16Be);
      CHECK(output == std::string{"\xF0\x9F\x98\x80"});
    }

    SECTION("UTF-16BE decodes BMP CJK text")
    {
      auto input = std::vector{std::byte{0x4F}, std::byte{0x60}, std::byte{0x59}, std::byte{0x7D}};
      auto output = convertToUtf8(input, Encoding::Utf16Be);
      CHECK(output == std::string{"\xE4\xBD\xA0\xE5\xA5\xBD"});
    }

    SECTION("UTF-16BE replaces malformed surrogates")
    {
      auto input = std::vector{
        std::byte{0xD8}, std::byte{0x3D}, std::byte{0x00}, std::byte{'A'}, std::byte{0xDC}, std::byte{0x00}};
      auto output = convertToUtf8(input, Encoding::Utf16Be);
      CHECK(output == std::string{"\xEF\xBF\xBD"
                                  "A"
                                  "\xEF\xBF\xBD"});
    }
  }

  TEST_CASE("ID3v2 - deunsynchronises v2.3 tags with the tag-level unsync flag", "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};

    // Latin-1 "Tit\xFFle" stored with an unsynchronisation escape after the FF.
    auto const storedTitle = textContent(Encoding::Latin1, std::string_view{"Tit\xFF\x00le", 7});
    appendFrame(body, "TIT2", false, 0x00, storedTitle, deunsynchronisedSize(storedTitle));

    auto const storedPicture = pictureContent(
      Bytes{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xBE}, std::byte{0xEF}});
    appendFrame(body, "APIC", false, 0x00, storedPicture, deunsynchronisedSize(storedPicture));
    body.insert(body.end(), 8, std::byte{}); // padding

    auto const optFrames = readFrames(makeHeader(3, 0x80, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title() == "Tit\xC3\xBFle"); // U+00FF encodes as C3 BF

    auto const& pictures = optFrames->coverArt().entries();
    REQUIRE(pictures.size() == 1);
    CHECK(pictures.front().type == PictureType::FrontCover);
    auto const expectedPicture =
      Bytes{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xFF}, std::byte{0xBE}, std::byte{0xEF}};
    CHECK(std::ranges::equal(pictures.front().bytes, expectedPicture));
  }

  TEST_CASE("ID3v2 - deunsynchronises v2.4 frames carrying the per-frame unsync flag", "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};

    // UTF-16LE "A\u00FFB": the LE code unit FF 00 for U+00FF is stored as FF 00 00.
    auto const storedTitle = Bytes{std::byte{0x01},
                                   std::byte{0xFF},
                                   std::byte{0xFE},
                                   std::byte{0x41},
                                   std::byte{0x00},
                                   std::byte{0xFF},
                                   std::byte{0x00},
                                   std::byte{0x00},
                                   std::byte{0x42},
                                   std::byte{0x00}};
    appendFrame(body, "TIT2", true, 0x02, storedTitle);

    // Frame sizes count stored bytes, so the next frame must still be reached.
    appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Second"));
    appendFrame(
      body,
      "APIC",
      true,
      0x02,
      pictureContent(
        Bytes{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xE0}}));

    auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title() == "A\xC3\xBF"
                                           "B");
    CHECK(optFrames->metadata().artist() == "Second");

    auto const& pictures = optFrames->coverArt().entries();
    REQUIRE(pictures.size() == 1);
    auto const expectedPicture =
      Bytes{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xE0}};
    CHECK(std::ranges::equal(pictures.front().bytes, expectedPicture));
  }

  TEST_CASE("ID3v2 - deunsynchronises v2.4 tags with the tag-level unsync flag", "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};

    // Tag-level unsynchronisation applies to every frame, including frames
    // that do not set the per-frame flag.
    appendFrame(body,
                "TALB",
                true,
                0x00,
                textContent(Encoding::Latin1,
                            std::string_view{"Al\xFF\x00"
                                             "bum",
                                             7}));
    appendFrame(body, "TIT2", true, 0x00, textContent(Encoding::Utf8, "Title"));
    appendFrame(
      body,
      "APIC",
      true,
      0x02,
      pictureContent(
        Bytes{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xE0}}));

    auto const optFrames = readFrames(makeHeader(4, 0x80, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().album() == "Al\xC3\xBF"
                                           "bum");
    CHECK(optFrames->metadata().title() == "Title");

    auto const& pictures = optFrames->coverArt().entries();
    REQUIRE(pictures.size() == 1);
    auto const expectedPicture =
      Bytes{std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xE0}};
    CHECK(std::ranges::equal(pictures.front().bytes, expectedPicture));
  }

  TEST_CASE("ID3v2 - skips the v2.4 data length indicator before frame data", "[media][unit][mpeg][id3v2]")
  {
    SECTION("data length indicator without unsynchronisation")
    {
      auto body = Bytes{};
      auto content = Bytes{};
      appendSyncSafe(content, 4); // deunsynchronised content length
      auto const data = textContent(Encoding::Utf8, "Art");
      content.insert(content.end(), data.begin(), data.end());
      appendFrame(body, "TIT2", true, 0x01, content);
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Art");
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("data length indicator combined with per-frame unsynchronisation")
    {
      // Stored: DLI(5) + UTF-8 encoding + "A" FF 00 "B"; after deunsynchronising
      // and skipping the indicator the data is 03 'A' FF 'B'.
      auto const content = Bytes{std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0x05},
                                 std::byte{0x03},
                                 std::byte{'A'},
                                 std::byte{0xFF},
                                 std::byte{0x00},
                                 std::byte{'B'}};
      auto body = Bytes{};
      appendFrame(body, "TPE2", true, 0x03, content);

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().albumArtist() == "A\xFF"
                                                   "B");
    }

    SECTION("truncated indicator drops only that frame")
    {
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x01, Bytes{std::byte{0x00}, std::byte{0x00}});
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title().empty());
      CHECK(optFrames->metadata().artist() == "Artist");
    }
  }

  TEST_CASE("ID3v2 - ignores unknown frames in unsynchronised v2.4 tags", "[media][unit][mpeg][id3v2]")
  {
    // Unknown frames are not dispatched, but their stored sizes still
    // advance the frame walk to the handled frames behind them.
    auto body = Bytes{};
    appendFrame(body, "XXXX", true, 0x02, Bytes{std::byte{0xFF}, std::byte{0x00}, std::byte{0xAB}});
    appendFrame(body, "TIT2", true, 0x00, textContent(Encoding::Utf8, "Title"));

    auto const optFrames = readFrames(makeHeader(4, 0x80, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title() == "Title");
  }

  TEST_CASE("ID3v2 - keeps the zero-copy view for unsynchronised frames without escapes", "[media][unit][mpeg][id3v2]")
  {
    // The unsynchronisation flag without a single stored FF 00 pair must not
    // allocate a copy; the frame still parses and the walk reaches the next one.
    auto body = Bytes{};
    appendFrame(body, "TIT2", true, 0x02, textContent(Encoding::Utf8, "Title"));
    appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

    auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title() == "Title");
    CHECK(optFrames->metadata().artist() == "Artist");
  }

  TEST_CASE("ID3v2 - skips v2.4 frames with a malformed data length indicator", "[media][unit][mpeg][id3v2]")
  {
    // A non-syncsafe indicator cannot encode a length, so the frame is
    // skipped while the rest of the tag still applies.
    auto content = Bytes{std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    auto const data = textContent(Encoding::Utf8, "Art");
    content.insert(content.end(), data.begin(), data.end());
    auto body = Bytes{};
    appendFrame(body, "TIT2", true, 0x01, content);
    appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

    auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title().empty());
    CHECK(optFrames->metadata().artist() == "Artist");
  }

  TEST_CASE("ID3v2 - skips the v2.4 grouping identity byte", "[media][unit][mpeg][id3v2]")
  {
    SECTION("grouping identity alone")
    {
      auto content = Bytes{};
      appendByte(content, 0x42); // group identifier
      auto const data = textContent(Encoding::Utf8, "Art");
      content.insert(content.end(), data.begin(), data.end());
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x40, content);
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Art");
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("grouping identity before the data length indicator")
    {
      auto content = Bytes{};
      appendByte(content, 0x42);  // group identifier
      appendSyncSafe(content, 4); // deunsynchronised content length
      auto const data = textContent(Encoding::Utf8, "Art");
      content.insert(content.end(), data.begin(), data.end());
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x41, content);
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Art");
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("grouping identity with per-frame unsynchronisation")
    {
      // Stored: group id, UTF-8 encoding, 'A', FF 00, 'B'; after
      // deunsynchronising and dropping the group byte the data is 03 'A' FF 'B'.
      auto const content =
        Bytes{std::byte{0x42}, std::byte{0x03}, std::byte{'A'}, std::byte{0xFF}, std::byte{0x00}, std::byte{'B'}};
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x42, content);
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "A\xFF"
                                             "B");
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("grouping identity with unsynchronised data and a data length indicator")
    {
      // Stored: group id, syncsafe indicator of the deunsynchronised length,
      // then Latin-1 "A\u00FFB" with FF stored as FF 00. After
      // deunsynchronising, dropping the group byte, and skipping the
      // indicator the data is 00 'A' FF 'B'.
      auto content = Bytes{std::byte{0x42}};
      appendSyncSafe(content, 4);
      content.insert(
        content.end(), {std::byte{0x00}, std::byte{'A'}, std::byte{0xFF}, std::byte{0x00}, std::byte{'B'}});
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x43, content);
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "A\xC3\xBF"
                                             "B");
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("grouping identity with empty content")
    {
      // The grouping flag on a frame with no content must not crash on the
      // empty guard and must leave the field unset; the tag walk continues.
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x40, Bytes{});
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title().empty());
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("grouping identity with only the group id")
    {
      // The single content byte is the group id; dropping it leaves empty
      // content, so the field stays unset and the tag walk continues.
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x40, Bytes{std::byte{0x42}});
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title().empty());
      CHECK(optFrames->metadata().artist() == "Artist");
    }
  }

  TEST_CASE("ID3v2 - skips v2.4 frames with compression or encryption", "[media][unit][mpeg][id3v2]")
  {
    // Compressed and encrypted frames have no decoder here, so each is
    // dropped while the rest of the tag still applies.
    auto body = Bytes{};
    appendFrame(body, "TIT2", true, 0x08, textContent(Encoding::Utf8, "Title"));
    appendFrame(body, "TALB", true, 0x04, textContent(Encoding::Utf8, "Album"));
    appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));

    auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title().empty());
    CHECK(optFrames->metadata().album().empty());
    CHECK(optFrames->metadata().artist() == "Artist");
  }

  TEST_CASE("ID3v2 - handles v2.3 frame format flags", "[media][unit][mpeg][id3v2]")
  {
    SECTION("grouping identity is dropped before the content")
    {
      auto content = Bytes{std::byte{0x42}};
      auto const data = textContent(Encoding::Latin1, "Art");
      content.insert(content.end(), data.begin(), data.end());
      auto body = Bytes{};
      appendFrame(body, "TIT2", false, 0x20, content);
      appendFrame(body, "TPE1", false, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(3, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Art");
      CHECK(optFrames->metadata().artist() == "Artist");
    }

    SECTION("compression and encryption skip their frames")
    {
      // Neither format has a decoder here, so both frames are dropped while
      // the rest of the tag still applies.
      auto body = Bytes{};
      appendFrame(body, "TIT2", false, 0x80, Bytes{std::byte{0x78}, std::byte{0x9C}, std::byte{0x00}});
      appendFrame(body, "TALB", false, 0x40, Bytes{std::byte{0x01}});
      appendFrame(body, "TPE1", false, 0x00, textContent(Encoding::Utf8, "Artist"));

      auto const optFrames = readFrames(makeHeader(3, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title().empty());
      CHECK(optFrames->metadata().album().empty());
      CHECK(optFrames->metadata().artist() == "Artist");
    }
  }

  TEST_CASE("ID3v2 - skips the v2.3 extended header before frames", "[media][unit][mpeg][id3v2]")
  {
    SECTION("extended header before a frame")
    {
      // A v2.3 extended header carrying a CRC: size field(4) + flags(2) +
      // padding size(4) + CRC(4) = 14 bytes, with the declared size 10. The
      // leading bytes (00 00 00 0A ...) do not form a zero-size frame, so the
      // old code that lacked extended-header skipping could not have consumed
      // this as one frame and still reach TIT2.
      auto body = Bytes{};
      appendUint32Be(body, 10);
      appendByte(body, 0x80); // CRC present flag
      appendByte(body, 0x00);
      appendUint32Be(body, 0); // padding size
      appendUint32Be(body, 0); // CRC
      appendFrame(body, "TIT2", false, 0x00, textContent(Encoding::Latin1, "Title"));

      auto const optFrames = readFrames(makeHeader(3, 0x40, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Title");
    }

    SECTION("extended header in a tag-level unsynchronised tag")
    {
      // Unsynchronisation covers the extended header too.
      auto body = Bytes{};
      appendUint32Be(body, 6);
      appendByte(body, 0);
      appendByte(body, 0);
      body.insert(body.end(), 4, std::byte{});
      auto const storedTitle = textContent(Encoding::Latin1, std::string_view{"Ti\xFF\x00tle", 7});
      appendFrame(body, "TIT2", false, 0x00, storedTitle, deunsynchronisedSize(storedTitle));

      auto const optFrames = readFrames(makeHeader(3, 0xC0, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Ti\xC3\xBFtle");
    }
  }

  TEST_CASE("ID3v2 - skips the v2.4 extended header before frames", "[media][unit][mpeg][id3v2]")
  {
    // The v2.4 size is syncsafe and includes the size field itself: size(4) +
    // flag byte count(1) + extended flags(1) = 6.
    auto body = Bytes{};
    appendSyncSafe(body, 6);
    appendByte(body, 1);
    appendByte(body, 0);
    appendFrame(body, "TIT2", true, 0x00, textContent(Encoding::Utf8, "Title"));

    auto const optFrames = readFrames(makeHeader(4, 0x40, body.size()), byteSpan(body));

    REQUIRE(optFrames);
    CHECK(optFrames->metadata().title() == "Title");
  }

  TEST_CASE("ID3v2 - rejects extended headers that overrun the tag", "[media][unit][mpeg][id3v2]")
  {
    SECTION("v2.3 extended header exceeds the tag body")
    {
      auto body = Bytes{};
      appendUint32Be(body, 100);
      appendByte(body, 0);
      appendByte(body, 0);

      CHECK_FALSE(readFrames(makeHeader(3, 0x40, body.size()), byteSpan(body)).has_value());
    }

    SECTION("v2.4 extended header exceeds the tag body")
    {
      auto body = Bytes{};
      appendSyncSafe(body, 64);
      appendByte(body, 1);
      appendByte(body, 0);

      CHECK_FALSE(readFrames(makeHeader(4, 0x40, body.size()), byteSpan(body)).has_value());
    }

    SECTION("v2.3 extended header below the minimum size")
    {
      // Declared size below the 6-byte floor but padded enough that the
      // overrun check alone would accept it.
      auto body = Bytes{};
      appendUint32Be(body, 4);
      appendByte(body, 0);
      appendByte(body, 0);
      body.insert(body.end(), 2, std::byte{});

      CHECK_FALSE(readFrames(makeHeader(3, 0x40, body.size()), byteSpan(body)).has_value());
    }

    SECTION("v2.4 extended header below the minimum size")
    {
      // Syncsafe size includes the size field: 5 bytes is below the 6-byte
      // floor but present, so only the size floor rejects it.
      auto body = Bytes{};
      appendSyncSafe(body, 5);
      appendByte(body, 0);

      CHECK_FALSE(readFrames(makeHeader(4, 0x40, body.size()), byteSpan(body)).has_value());
    }

    SECTION("v2.4 extended header size is not syncsafe")
    {
      auto const body =
        Bytes{std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0x06}, std::byte{0x01}, std::byte{0x00}};

      CHECK_FALSE(readFrames(makeHeader(4, 0x40, body.size()), byteSpan(body)).has_value());
    }

    SECTION("truncated size field")
    {
      auto const body = Bytes{std::byte{0x00}, std::byte{0x00}};

      CHECK_FALSE(readFrames(makeHeader(3, 0x40, body.size()), byteSpan(body)).has_value());
      CHECK_FALSE(readFrames(makeHeader(4, 0x40, body.size()), byteSpan(body)).has_value());
    }
  }

  TEST_CASE("ID3v2 - applies v2.4 NUL-separated text values in order", "[media][unit][mpeg][id3v2]")
  {
    SECTION("UTF-8")
    {
      auto body = Bytes{};
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, std::string_view{"A\0B", 3}));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      // Later values overwrite earlier ones, like repeated frames.
      CHECK(optFrames->metadata().artist() == "B");
    }

    SECTION("Latin-1")
    {
      auto body = Bytes{};
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Latin1, std::string_view{"A\0B", 3}));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().artist() == "B");
    }

    SECTION("UTF-16 with BOM")
    {
      auto const content = Bytes{std::byte{0x01},
                                 std::byte{0xFE},
                                 std::byte{0xFF},
                                 std::byte{0x00},
                                 std::byte{0x41},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0x42},
                                 std::byte{0x00},
                                 std::byte{0x00}};
      auto body = Bytes{};
      appendFrame(body, "TPE1", true, 0x00, content);

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().artist() == "B");
    }

    SECTION("UTF-16 LE multi-value with per-value BOMs")
    {
      // Each value carries its own LE BOM. The whole-frame BOM is stripped
      // during conversion, but the interior BOM survives the NUL split as a
      // UTF-8-encoded U+FEFF (EF BB BF) unless forEachTextValue strips it.
      // encoding | BOM 'A' | NUL sep | BOM 'B' | NUL term
      auto const content = Bytes{std::byte{0x01},
                                 std::byte{0xFF},
                                 std::byte{0xFE},
                                 std::byte{0x41},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0xFF},
                                 std::byte{0xFE},
                                 std::byte{0x42},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0x00}};
      auto body = Bytes{};
      appendFrame(body, "TPE1", true, 0x00, content);

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().artist() == "B");
    }

    SECTION("UTF-16 BE multi-value with per-value BOMs")
    {
      // Same shape with BE BOMs (FE FF); the second value's interior BOM is
      // also stripped.
      auto const content = Bytes{std::byte{0x01},
                                 std::byte{0xFE},
                                 std::byte{0xFF},
                                 std::byte{0x00},
                                 std::byte{0x41},
                                 std::byte{0x00},
                                 std::byte{0x00},
                                 std::byte{0xFE},
                                 std::byte{0xFF},
                                 std::byte{0x00},
                                 std::byte{0x42},
                                 std::byte{0x00},
                                 std::byte{0x00}};
      auto body = Bytes{};
      appendFrame(body, "TPE1", true, 0x00, content);

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().artist() == "B");
    }

    SECTION("trailing terminator keeps the single value")
    {
      auto body = Bytes{};
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, std::string_view{"Solo\0\0", 6}));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().artist() == "Solo");
    }

    SECTION("numeric fields apply the last parseable value")
    {
      auto body = Bytes{};
      appendFrame(body,
                  "TDRC",
                  true,
                  0x00,
                  textContent(Encoding::Utf8,
                              std::string_view{"2023\0"
                                               "2024",
                                               9}));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().year() == 2024);
    }

    SECTION("TXXX values")
    {
      auto body = Bytes{};
      appendFrame(body, "TXXX", true, 0x00, textContent(Encoding::Utf8, std::string_view{"conductor\0A\0B", 13}));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      auto const credits = optFrames->metadata().credits();
      REQUIRE(credits.size() == 2);
      CHECK(credits[0].name == "A");
      CHECK(credits[0].kind == CreditKind::Conductor);
      CHECK(credits[0].role.empty());
      CHECK(credits[1].name == "B");
      CHECK(credits[1].kind == CreditKind::Conductor);
      CHECK(credits[1].role.empty());
    }
  }

  TEST_CASE("ID3v2 - keeps v2.3 text values at the first terminator", "[media][unit][mpeg][id3v2]")
  {
    SECTION("text frames")
    {
      auto body = Bytes{};
      appendFrame(body, "TPE1", false, 0x00, textContent(Encoding::Latin1, std::string_view{"A\0B", 3}));

      auto const optFrames = readFrames(makeHeader(3, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().artist() == "A");
    }

    SECTION("TXXX values")
    {
      auto body = Bytes{};
      appendFrame(body, "TXXX", false, 0x00, textContent(Encoding::Latin1, std::string_view{"conductor\0A\0B", 13}));

      auto const optFrames = readFrames(makeHeader(3, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      auto const credits = optFrames->metadata().credits();
      REQUIRE(credits.size() == 1);
      CHECK(credits[0].name == "A");
      CHECK(credits[0].kind == CreditKind::Conductor);
      CHECK(credits[0].role.empty());
    }
  }

  TEST_CASE("ID3v2 credits - v2.3 TPE3 and TXXX stop at the first terminator without splitting slash text",
            "[media][unit][mpeg][id3v2]")
  {
    auto body = Bytes{};
    appendFrame(body, "TPE3", false, 0, textContent(Encoding::Latin1, std::string_view{"A/B\0C", 5}));
    appendFrame(body, "TXXX", false, 0, textContent(Encoding::Latin1, std::string_view{"soloist\0D/E\0F", 13}));
    auto const optFrames = readFrames(makeHeader(3, 0, body.size()), byteSpan(body));
    REQUIRE(optFrames);
    auto const credits = optFrames->metadata().credits();
    REQUIRE(credits.size() == 2);
    CHECK(credits[0].name == "A/B");
    CHECK(credits[0].kind == CreditKind::Conductor);
    CHECK(credits[0].role.empty());
    CHECK(credits[1].name == "D/E");
    CHECK(credits[1].kind == CreditKind::Soloist);
    CHECK(credits[1].role.empty());
  }

  TEST_CASE("ID3v2 - parses ordinary v2.3 and v2.4 frames", "[media][unit][mpeg][id3v2]")
  {
    SECTION("v2.3")
    {
      auto body = Bytes{};
      appendFrame(body, "TIT2", false, 0x00, textContent(Encoding::Latin1, "Title"));
      appendFrame(body, "TPE1", false, 0x00, textContent(Encoding::Latin1, "Artist"));
      appendFrame(body, "TYER", false, 0x00, textContent(Encoding::Latin1, "2024"));
      appendFrame(body, "TRCK", false, 0x00, textContent(Encoding::Latin1, "1/10"));
      appendFrame(body, "APIC", false, 0x00, pictureContent(Bytes{std::byte{0xAB}, std::byte{0xCD}}));
      body.insert(body.end(), 8, std::byte{}); // padding

      auto const optFrames = readFrames(makeHeader(3, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Title");
      CHECK(optFrames->metadata().artist() == "Artist");
      CHECK(optFrames->metadata().year() == 2024);
      CHECK(optFrames->metadata().trackNumber() == 1);
      CHECK(optFrames->metadata().trackTotal() == 10);
      REQUIRE(optFrames->coverArt().entries().size() == 1);
      CHECK(optFrames->coverArt().entries().front().bytes.size() == 2);
    }

    SECTION("v2.4")
    {
      auto body = Bytes{};
      appendFrame(body, "TIT2", true, 0x00, textContent(Encoding::Utf8, "Title"));
      appendFrame(body, "TPE1", true, 0x00, textContent(Encoding::Utf8, "Artist"));
      appendFrame(body, "TDRC", true, 0x00, textContent(Encoding::Utf8, "2024-05-17"));

      auto const optFrames = readFrames(makeHeader(4, 0x00, body.size()), byteSpan(body));

      REQUIRE(optFrames);
      CHECK(optFrames->metadata().title() == "Title");
      CHECK(optFrames->metadata().artist() == "Artist");
      CHECK(optFrames->metadata().year() == 2024);
    }
  }

  TEST_CASE("ID3v2 - explicit work frames outrank grouping aliases regardless of frame order",
            "[media][unit][mpeg][id3v2]")
  {
    auto const txxxContent = [](Encoding encoding, std::string_view description, std::string_view value)
    {
      auto text = std::string{description};
      text.push_back('\0');
      text.append(value);
      return textContent(encoding, text);
    };

    // Copy the view out; the builder dies with the lambda.
    auto const readWork = [](bool v24, Bytes const& body) -> std::string
    {
      auto const optFrames = readFrames(makeHeader(v24 ? 4 : 3, 0x00, body.size()), byteSpan(body));
      REQUIRE(optFrames);
      return std::string{optFrames->metadata().work()};
    };

    // All six ASCII whitespace characters.
    auto const blank = std::string{" \t\n\r\f\v"};

    auto const appendTxxxWork = [&](bool v24, Bytes& body, std::string_view value)
    { appendFrame(body, "TXXX", v24, 0x00, txxxContent(v24 ? Encoding::Utf8 : Encoding::Latin1, "work", value)); };

    auto const txxxWorkFrame = [&](bool v24, std::string_view value)
    {
      auto body = Bytes{};
      appendTxxxWork(v24, body, value);
      return body;
    };

    auto const appendTit1 = [&](bool v24, Bytes& body, std::string_view value)
    { appendFrame(body, "TIT1", v24, 0x00, textContent(v24 ? Encoding::Utf8 : Encoding::Latin1, value)); };

    auto const appendTxxxGrouping = [&](bool v24, Bytes& body, std::string_view value)
    { appendFrame(body, "TXXX", v24, 0x00, txxxContent(v24 ? Encoding::Utf8 : Encoding::Latin1, "grouping", value)); };

    SECTION("TXXX:WORK wins over TIT1 and TXXX:GROUPING in both orders")
    {
      auto workFirst = txxxWorkFrame(true, "TxxxWork");
      appendTit1(true, workFirst, "Tit1Work");
      appendTxxxGrouping(true, workFirst, "TxxxGrouping");

      auto groupingFirst = Bytes{};
      appendTxxxGrouping(true, groupingFirst, "TxxxGrouping");
      appendTit1(true, groupingFirst, "Tit1Work");
      auto const workFrame = txxxWorkFrame(true, "TxxxWork");
      groupingFirst.insert(groupingFirst.end(), workFrame.begin(), workFrame.end());

      CHECK(readWork(true, workFirst) == "TxxxWork");
      CHECK(readWork(true, groupingFirst) == "TxxxWork");
    }

    SECTION("TIT1 wins over TXXX:GROUPING in both orders")
    {
      auto tit1First = Bytes{};
      appendTit1(true, tit1First, "Tit1Work");
      appendTxxxGrouping(true, tit1First, "TxxxGrouping");

      auto groupingFirst = Bytes{};
      appendTxxxGrouping(true, groupingFirst, "TxxxGrouping");
      appendTit1(true, groupingFirst, "Tit1Work");

      CHECK(readWork(true, tit1First) == "Tit1Work");
      CHECK(readWork(true, groupingFirst) == "Tit1Work");
    }

    SECTION("each single source still fills the work field")
    {
      CHECK(readWork(true, txxxWorkFrame(true, "TxxxWork")) == "TxxxWork");

      auto tit1Only = Bytes{};
      appendTit1(true, tit1Only, "Tit1Work");
      CHECK(readWork(true, tit1Only) == "Tit1Work");

      auto groupingOnly = Bytes{};
      appendTxxxGrouping(true, groupingOnly, "TxxxGrouping");
      CHECK(readWork(true, groupingOnly) == "TxxxGrouping");
    }

    SECTION("a blank higher source does not suppress the lower one in either order")
    {
      // TXXX:WORK blank around TIT1.
      auto workBlankBeforeTit1 = Bytes{};
      appendTxxxWork(true, workBlankBeforeTit1, blank);
      appendTit1(true, workBlankBeforeTit1, "Tit1Work");
      CHECK(readWork(true, workBlankBeforeTit1) == "Tit1Work");

      auto tit1BeforeBlankWork = Bytes{};
      appendTit1(true, tit1BeforeBlankWork, "Tit1Work");
      appendTxxxWork(true, tit1BeforeBlankWork, blank);
      CHECK(readWork(true, tit1BeforeBlankWork) == "Tit1Work");

      // TIT1 blank around TXXX:GROUPING.
      auto tit1BlankBeforeGrouping = Bytes{};
      appendTit1(true, tit1BlankBeforeGrouping, blank);
      appendTxxxGrouping(true, tit1BlankBeforeGrouping, "TxxxGrouping");
      CHECK(readWork(true, tit1BlankBeforeGrouping) == "TxxxGrouping");

      auto groupingBeforeBlankTit1 = Bytes{};
      appendTxxxGrouping(true, groupingBeforeBlankTit1, "TxxxGrouping");
      appendTit1(true, groupingBeforeBlankTit1, blank);
      CHECK(readWork(true, groupingBeforeBlankTit1) == "TxxxGrouping");
    }

    SECTION("all-blank work sources leave the field empty")
    {
      auto body = Bytes{};
      appendTxxxWork(true, body, blank);
      appendTit1(true, body, blank);
      appendTxxxGrouping(true, body, blank);

      // An empty builder work is the observable form of no work callback:
      // Content::visit skips empty text values.
      CHECK(readWork(true, body).empty());
    }

    SECTION("repeated same-source frames keep the last nonempty value")
    {
      auto body = txxxWorkFrame(true, "First");
      auto const second = txxxWorkFrame(true, "Second");
      body.insert(body.end(), second.begin(), second.end());
      CHECK(readWork(true, body) == "Second");

      auto const blankWork = txxxWorkFrame(true, blank);
      body.insert(body.end(), blankWork.begin(), blankWork.end());
      CHECK(readWork(true, body) == "Second");
    }

    SECTION("a v2.4 TIT1 frame keeps the multi-value last-wins convention")
    {
      auto body = Bytes{};
      appendTit1(true, body, std::string_view{"A\0B", 3});
      CHECK(readWork(true, body) == "B");
    }

    SECTION("v2.3 frames keep the same precedence")
    {
      auto workFirst = txxxWorkFrame(false, "TxxxWork");
      appendTit1(false, workFirst, "Tit1Work");
      appendTxxxGrouping(false, workFirst, "TxxxGrouping");

      auto groupingFirst = Bytes{};
      appendTxxxGrouping(false, groupingFirst, "TxxxGrouping");
      appendTit1(false, groupingFirst, "Tit1Work");
      auto const workFrame = txxxWorkFrame(false, "TxxxWork");
      groupingFirst.insert(groupingFirst.end(), workFrame.begin(), workFrame.end());

      CHECK(readWork(false, workFirst) == "TxxxWork");
      CHECK(readWork(false, groupingFirst) == "TxxxWork");
    }
  }
} // namespace ao::media::file::mpeg::id3v2::test
