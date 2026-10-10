// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/file/TestFile.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::media::file::wav::test
{
  using File = ao::media::file::test::TestFile;

  namespace
  {
    ao::media::file::test::RecordedContent readContent(File const& file)
    {
      auto res = file.readContent();
      REQUIRE(res);
      return *res;
    }

    void appendId3Be32(std::vector<std::uint8_t>& output, std::uint32_t value)
    {
      output.push_back(static_cast<std::uint8_t>(value >> 24));
      output.push_back(static_cast<std::uint8_t>(value >> 16));
      output.push_back(static_cast<std::uint8_t>(value >> 8));
      output.push_back(static_cast<std::uint8_t>(value));
    }

    // Appends a v2.3 ID3 tag header: version 2.3, no flags, syncsafe body size.
    void appendId3v23Header(std::vector<std::uint8_t>& output, std::size_t bodySize)
    {
      output.insert(output.end(), {'I', 'D', '3', 3, 0, 0});
      auto const size = static_cast<std::uint32_t>(bodySize);
      output.push_back(static_cast<std::uint8_t>((size >> 21U) & 0x7FU));
      output.push_back(static_cast<std::uint8_t>((size >> 14U) & 0x7FU));
      output.push_back(static_cast<std::uint8_t>((size >> 7U) & 0x7FU));
      output.push_back(static_cast<std::uint8_t>(size & 0x7FU));
    }

    // Appends a v2.3 Latin-1 text frame: 10-byte header, encoding byte, text.
    void appendLatin1TextFrame(std::vector<std::uint8_t>& output, std::string_view id, std::string_view text)
    {
      for (auto const character : id)
      {
        output.push_back(static_cast<std::uint8_t>(character));
      }

      appendId3Be32(output, static_cast<std::uint32_t>(text.size() + 1));
      output.push_back(0); // status flags
      output.push_back(0); // format flags
      output.push_back(0); // Latin-1

      for (auto const character : text)
      {
        output.push_back(static_cast<std::uint8_t>(character));
      }
    }

    void appendTxxxFrame(std::vector<std::uint8_t>& output, std::string_view description, std::string_view value)
    {
      auto text = std::string{description};
      text.push_back('\0');
      text.append(value);
      appendLatin1TextFrame(output, "TXXX", text);
    }

    void appendSyncSafeSize(std::vector<std::uint8_t>& output, std::size_t size)
    {
      for (auto const shift : {21U, 14U, 7U, 0U})
      {
        output.push_back(static_cast<std::uint8_t>((size >> shift) & 0x7FU));
      }
    }

    std::vector<std::uint8_t> makeCreditTag(std::string_view storedLatin1Text)
    {
      auto tag = std::vector<std::uint8_t>{'I', 'D', '3', 4, 0, 0};
      appendSyncSafeSize(tag, storedLatin1Text.size() + 11U);
      tag.insert(tag.end(), {'T', 'M', 'C', 'L'});
      appendSyncSafeSize(tag, storedLatin1Text.size() + 1U);
      tag.push_back(0);    // Frame status flags.
      tag.push_back(0x02); // Frame unsynchronisation; the decoded text must be copied.
      tag.push_back(0);    // Latin-1.

      for (auto const character : storedLatin1Text)
      {
        tag.push_back(static_cast<std::uint8_t>(character));
      }

      return tag;
    }

    std::string readWorkAcrossChunks(std::vector<std::vector<std::uint8_t>> const& frameGroups)
    {
      auto chunks = std::vector<ao::test::wav::Chunk>{};
      chunks.reserve(frameGroups.size());

      for (std::size_t index = 0; index < frameGroups.size(); ++index)
      {
        auto payload = std::vector<std::uint8_t>{};
        appendId3v23Header(payload, frameGroups[index].size());
        payload.insert(payload.end(), frameGroups[index].begin(), frameGroups[index].end());
        auto const chunkId =
          index % 2U == 0U ? std::array<char, 4>{'i', 'd', '3', ' '} : std::array<char, 4>{'I', 'D', '3', ' '};
        chunks.push_back({.id = chunkId, .payload = std::move(payload)});
      }

      auto const data = ao::test::wav::makeWav({.extraChunks = std::move(chunks)});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      return std::string{readContent(file).text(TextField::Work)};
    }

    std::vector<std::uint8_t> workFrames(std::string_view value)
    {
      auto frames = std::vector<std::uint8_t>{};
      appendTxxxFrame(frames, "work", value);
      return frames;
    }

    std::vector<std::uint8_t> tit1Frames(std::string_view value)
    {
      auto frames = std::vector<std::uint8_t>{};
      appendLatin1TextFrame(frames, "TIT1", value);
      return frames;
    }

    std::vector<std::uint8_t> groupingFrames(std::string_view value)
    {
      auto frames = std::vector<std::uint8_t>{};
      appendTxxxFrame(frames, "grouping", value);
      return frames;
    }
  } // namespace

  TEST_CASE("WAV File - emits real INFO tag fields", "[media][unit][wav][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.wav")};
    auto content = readContent(file);
    auto const& metadata = content;

    CHECK(metadata.text(TextField::Title) == "Test Title");
    CHECK(metadata.text(TextField::Artist) == "Test Artist");
    CHECK(metadata.text(TextField::Album) == "Test Album");
    CHECK(metadata.text(TextField::Genre) == "Rock");
    CHECK(metadata.number(NumberField::Year) == 2024);
    CHECK(metadata.credits().empty());
    CHECK_FALSE(std::ranges::contains(metadata.events(),
                                      ao::media::file::test::RecordedContent::CallbackEvent{
                                        .kind = ao::media::file::test::RecordedContent::CallbackKind::Credits}));
  }

  TEST_CASE("WAV File - reads real PCM audio properties", "[media][unit][wav][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("hires.wav")};
    auto content = readContent(file);
    auto const& prop = content;

    CHECK(prop.codec() == AudioCodec::Wav);
    CHECK(prop.sampleRate() == 96000);
    CHECK(prop.channels() == 2);
    CHECK(prop.bitDepth() == 24);
    CHECK(prop.duration() >= std::chrono::milliseconds{950});
    CHECK(prop.duration() <= std::chrono::milliseconds{1050});
    CHECK(prop.bitrate() >= 4000000);
  }

  TEST_CASE("WAV File - audio payload range points at real data chunk", "[media][unit][wav][file]")
  {
    auto const fixture = audio::test::requireAudioFixture("basic_metadata.wav");
    auto const bytes = audio::test::readFileBytes(fixture);
    auto const file = File{fixture};
    auto rangeRes = file.audioPayload();

    REQUIRE(rangeRes);
    auto const range = *rangeRes;
    REQUIRE(range.offset > 0);
    REQUIRE(range.offset + range.bytes.size() <= bytes.size());
    CHECK(range.bytes.size() == static_cast<std::size_t>(44100U) * 2U * 2U);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[0]) == bytes[range.offset]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[1]) == bytes[range.offset + 1U]);
  }

  TEST_CASE("WAV File - audio payload signature ignores INFO metadata changes", "[media][unit][wav][file]")
  {
    auto firstData = ao::test::wav::makeWav({
      .audioData = {0x00, 0x00, 0x01, 0x00},
      .infoFields = {{{.id = {'I', 'N', 'A', 'M'}, .value = "Before"}}},
    });
    auto secondData = ao::test::wav::makeWav({
      .audioData = {0x00, 0x00, 0x01, 0x00},
      .infoFields = {{{.id = {'I', 'N', 'A', 'M'}, .value = "After"}}},
    });
    auto const firstTemp = ao::test::TempFile{firstData, ".wav"};
    auto const secondTemp = ao::test::TempFile{secondData, ".wav"};
    auto const firstFile = File{firstTemp.path};
    auto const secondFile = File{secondTemp.path};

    auto firstPayloadRes = firstFile.audioPayload();
    auto secondPayloadRes = secondFile.audioPayload();
    REQUIRE(firstPayloadRes);
    REQUIRE(secondPayloadRes);

    CHECK(utility::xxh3Hash128(firstPayloadRes->bytes) == utility::xxh3Hash128(secondPayloadRes->bytes));
  }

  TEST_CASE("WAV File - preserves ID3 APIC cover art", "[media][unit][wav][file]")
  {
    auto const id3 = ao::test::wav::makeId3WithPicture(std::array<std::uint8_t, 3>{0x12, 0x34, 0x56});
    auto const data = ao::test::wav::makeWav({
      .extraChunks = {{{.id = {'i', 'd', '3', ' '}, .payload = id3}}},
    });
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const file = File{temp.path};
    auto const content = readContent(file);

    auto const& pictures = content.pictures();
    REQUIRE(pictures.size() == 1);
    CHECK(pictures.front().type == PictureType::FrontCover);
    REQUIRE(pictures.front().bytes.size() == 3);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[0]) == 0x12);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[1]) == 0x34);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[2]) == 0x56);
  }

  TEST_CASE("WAV File - preserves ID3 content from unsynchronised chunks", "[media][unit][wav][file]")
  {
    // The ID3 reader deunsynchronises the chunk into storage owned by the
    // per-chunk builder, which dies with it; the copied picture bytes must
    // stay readable here, after that builder is gone.
    auto const storedPicture = std::array<std::uint8_t, 5>{0x12, 0x34, 0xFF, 0x00, 0x56};
    auto const storedTitle = std::string_view{"Ti\xFF\x00"
                                              "tle",
                                              7};
    auto const id3 = ao::test::wav::makeUnsyncId3WithPictureAndTitle(storedPicture, storedTitle);
    auto const data = ao::test::wav::makeWav({
      .extraChunks = {{{.id = {'i', 'd', '3', ' '}, .payload = id3}}},
    });
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const file = File{temp.path};
    auto const content = readContent(file);

    // Latin-1 0xFF converts to UTF-8 C3 BF.
    CHECK(content.text(TextField::Title) == "Ti\xC3\xBF"
                                            "tle");

    auto const& pictures = content.pictures();
    REQUIRE(pictures.size() == 1);
    CHECK(pictures.front().type == PictureType::FrontCover);
    REQUIRE(pictures.front().bytes.size() == 4);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[0]) == 0x12);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[1]) == 0x34);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[2]) == 0xFF);
    CHECK(std::to_integer<std::uint8_t>(pictures.front().bytes[3]) == 0x56);
  }

  TEST_CASE("WAV File - embedded ID3 work survives the copy at its resolved precedence", "[media][unit][wav][file]")
  {
    auto const readWorkFromId3 = [](std::vector<std::uint8_t> const& frames) -> std::string
    {
      auto id3 = std::vector<std::uint8_t>{};
      appendId3v23Header(id3, frames.size());
      id3.insert(id3.end(), frames.begin(), frames.end());

      auto const data = ao::test::wav::makeWav({
        .extraChunks = {{{.id = {'i', 'd', '3', ' '}, .payload = id3}}},
      });
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      // Copy the view out; the file and content die with the lambda.
      return std::string{readContent(file).text(TextField::Work)};
    };

    SECTION("TXXX:WORK wins over TIT1 and TXXX:GROUPING in the copied tag")
    {
      auto workFirst = std::vector<std::uint8_t>{};
      appendTxxxFrame(workFirst, "work", "TxxxWork");
      appendLatin1TextFrame(workFirst, "TIT1", "Tit1Work");
      appendTxxxFrame(workFirst, "grouping", "TxxxGrouping");
      CHECK(readWorkFromId3(workFirst) == "TxxxWork");

      auto groupingFirst = std::vector<std::uint8_t>{};
      appendTxxxFrame(groupingFirst, "grouping", "TxxxGrouping");
      appendLatin1TextFrame(groupingFirst, "TIT1", "Tit1Work");
      appendTxxxFrame(groupingFirst, "work", "TxxxWork");
      CHECK(readWorkFromId3(groupingFirst) == "TxxxWork");
    }

    SECTION("TIT1 wins over the grouping alias in the copied tag")
    {
      auto tit1First = std::vector<std::uint8_t>{};
      appendLatin1TextFrame(tit1First, "TIT1", "Tit1Work");
      appendTxxxFrame(tit1First, "grouping", "TxxxGrouping");
      CHECK(readWorkFromId3(tit1First) == "Tit1Work");

      auto groupingFirst = std::vector<std::uint8_t>{};
      appendTxxxFrame(groupingFirst, "grouping", "TxxxGrouping");
      appendLatin1TextFrame(groupingFirst, "TIT1", "Tit1Work");
      CHECK(readWorkFromId3(groupingFirst) == "Tit1Work");
    }

    SECTION("a grouping-only fallback survives the copy")
    {
      auto frames = std::vector<std::uint8_t>{};
      appendTxxxFrame(frames, "grouping", "TxxxGrouping");
      CHECK(readWorkFromId3(frames) == "TxxxGrouping");
    }
  }

  TEST_CASE("WAV File - preserves work rank across multiple embedded ID3 chunks", "[media][unit][wav][file]")
  {
    // Each candidate is its own id3/ID3 chunk. Dropping the copied rank would
    // let a later lower-ranked chunk replace an earlier winner.
    SECTION("mixed and reverse chunk order keep the highest rank")
    {
      CHECK(readWorkAcrossChunks({workFrames("TxxxWork"), tit1Frames("Tit1Work"), groupingFrames("TxxxGrouping")}) ==
            "TxxxWork");
      CHECK(readWorkAcrossChunks({groupingFrames("TxxxGrouping"), tit1Frames("Tit1Work"), workFrames("TxxxWork")}) ==
            "TxxxWork");
      CHECK(readWorkAcrossChunks({tit1Frames("Tit1Work"), workFrames("TxxxWork"), groupingFrames("TxxxGrouping")}) ==
            "TxxxWork");
      CHECK(readWorkAcrossChunks({groupingFrames("TxxxGrouping"), tit1Frames("Tit1Work")}) == "Tit1Work");
    }

    SECTION("a later equal-rank chunk replaces the earlier work")
    {
      CHECK(readWorkAcrossChunks({workFrames("EarlierWork"), workFrames("LaterWork")}) == "LaterWork");
      CHECK(readWorkAcrossChunks(
              {tit1Frames("EarlierTit1"), tit1Frames("LaterTit1"), groupingFrames("TxxxGrouping")}) == "LaterTit1");
    }
  }

  TEST_CASE("WAV File - public visitor owns ordered credits after scalar and cover callbacks",
            "[media][unit][wav][file]")
  {
    using Event = ao::media::file::test::RecordedContent::CallbackEvent;
    using Kind = ao::media::file::test::RecordedContent::CallbackKind;
    using Credit = ao::media::file::test::RecordedContent::Credit;

    auto const owned = []
    {
      auto const picture = std::array<std::uint8_t, 3>{0x12, 0x34, 0x56};
      auto const pictureTag = ao::test::wav::makeId3WithPicture(picture);
      // The FF 00 pair is a frame-unsynchronisation escape. The converted name
      // exists only in storage copied out of the per-chunk builder.
      constexpr auto kCreditText = std::to_array("piano\0Ada\0piano\0Ada\0\0Bob\0violin\0A\xFF\0\xE0");
      auto const creditTag = makeCreditTag(std::string_view{kCreditText.data(), kCreditText.size() - 1});
      auto const data = ao::test::wav::makeWav({
        .audioData = {0x00, 0x00},
        .infoFields = {{{.id = {'I', 'N', 'A', 'M'}, .value = "Title"}}},
        .extraChunks = {{.id = {'i', 'd', '3', ' '}, .payload = pictureTag},
                        {.id = {'I', 'D', '3', ' '}, .payload = creditTag}},
      });
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      auto content = readContent(file);

      CHECK(content.text(TextField::Title) == "Title");
      CHECK(content.events() == std::vector<Event>{
                                  {Kind::Text, static_cast<std::uint8_t>(TextField::Title)},
                                  {Kind::Codec},
                                  {Kind::Duration},
                                  {Kind::Bitrate},
                                  {Kind::SampleRate},
                                  {Kind::Channels},
                                  {Kind::BitDepth},
                                  {Kind::Picture, static_cast<std::uint8_t>(PictureType::FrontCover)},
                                  {Kind::Credits},
                                });
      REQUIRE(content.pictures().size() == 1);
      CHECK(content.pictures().front().bytes.size() == picture.size());
      return content.credits();
    }();

    auto const expected = std::array<Credit, 4>{
      Credit{.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
      Credit{.name = "Ada", .kind = CreditKind::Performer, .role = "piano"},
      Credit{.name = "Bob", .kind = CreditKind::Performer},
      Credit{.name = "A\xC3\xBF\xC3\xA0", .kind = CreditKind::Performer, .role = "violin"},
    };
    REQUIRE(owned.size() == expected.size());
    CHECK(owned[0] == expected[0]);
    CHECK(owned[1] == expected[1]);
    CHECK(owned[2] == expected[2]);
    CHECK(owned[3] == expected[3]);
  }

  TEST_CASE("WAV File - rejects empty audio data through the content API", "[media][unit][wav][file]")
  {
    auto data = ao::test::wav::makeWav({.audioData = {}});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const file = File{temp.path};
    auto res = file.readContent();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::CorruptData);
    CHECK(res.error().message == "WAV file has no audio data");
  }

  TEST_CASE("WAV File - discards malformed optional ID3 metadata", "[media][unit][wav][file]")
  {
    auto malformedId3 =
      std::vector<std::uint8_t>{'I', 'D', '3', 3, 0, 0,   0,   0,   0,   22,  'T', 'I', 'T', '2', 0, 0,
                                0,   2,   0,   0, 0, 'A', 'T', 'P', 'E', '1', 0,   0,   0,   100, 0, 0};
    auto data = ao::test::wav::makeWav({
      .extraChunks = {{{.id = {'i', 'd', '3', ' '}, .payload = malformedId3}}},
    });
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const file = File{temp.path};
    auto res = file.readContent();

    REQUIRE(res);
    CHECK(res->text(TextField::Title).empty());
    CHECK(res->codec() == AudioCodec::Wav);
  }

  TEST_CASE("WAV File - malformed LIST discards fields parsed before the error", "[media][unit][wav][file]")
  {
    auto const fields = std::vector<ao::test::wav::InfoField>{{.id = {'I', 'N', 'A', 'M'}, .value = "Partial"}};
    auto info = ao::test::wav::makeInfoList(fields);
    info.insert(info.end(), {'I', 'A', 'R', 'T'});
    ao::test::wav::appendLe32(info, 100);
    auto data = ao::test::wav::makeWav({
      .extraChunks = {{{.id = {'L', 'I', 'S', 'T'}, .payload = info}}},
    });
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const file = File{temp.path};
    auto res = file.readContent();

    REQUIRE(res);
    CHECK(res->text(TextField::Title).empty());
    CHECK(res->codec() == AudioCodec::Wav);
  }
} // namespace ao::media::file::wav::test
