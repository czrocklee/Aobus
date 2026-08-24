// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/file/mpeg/id3v2/Layout.h"
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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ao::media::file::wav::test
{
  using File = ao::media::file::test::TestFile;

  namespace
  {
    namespace id3v2 = ao::media::file::mpeg::id3v2;

    ao::media::file::test::RecordedContent readContent(File const& file)
    {
      auto result = file.readContent();
      REQUIRE(result);
      return *result;
    }

    void addPictureFrame(std::vector<std::uint8_t>& data, std::array<std::uint8_t, 3> const imageData)
    {
      auto body = std::vector<std::uint8_t>{0}; // Latin1
      body.insert(body.end(), {'i', 'm', 'a', 'g', 'e', '/', 'p', 'n', 'g', 0});
      body.insert(body.end(), {3, 0}); // Front cover and empty description
      body.insert(body.end(), imageData.begin(), imageData.end());

      auto frame = id3v2::V23CommonFrameLayout{};
      std::memcpy(frame.id.data(), "APIC", frame.id.size());
      frame.size = static_cast<std::uint32_t>(body.size());
      auto const* const frameBytes = reinterpret_cast<std::uint8_t const*>(&frame);
      data.insert(data.end(), frameBytes, frameBytes + sizeof(frame));
      data.insert(data.end(), body.begin(), body.end());
    }

    std::vector<std::uint8_t> makeId3WithPicture()
    {
      auto body = std::vector<std::uint8_t>{};
      addPictureFrame(body, {0x12, 0x34, 0x56});

      auto header = id3v2::HeaderLayout{};
      std::memcpy(header.id.data(), "ID3", header.id.size());
      header.majorVersion = 3;

      auto const size = static_cast<std::uint32_t>(body.size());
      header.size.data[0] = (size >> 21U) & 0x7FU;
      header.size.data[1] = (size >> 14U) & 0x7FU;
      header.size.data[2] = (size >> 7U) & 0x7FU;
      header.size.data[3] = size & 0x7FU;

      auto data = std::vector<std::uint8_t>{};
      auto const* const headerBytes = reinterpret_cast<std::uint8_t const*>(&header);
      data.insert(data.end(), headerBytes, headerBytes + sizeof(header));
      data.insert(data.end(), body.begin(), body.end());
      return data;
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

  TEST_CASE("WAV File - preserves ID3 APIC cover art", "[media][regression][wav]")
  {
    auto const id3 = makeId3WithPicture();
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

  TEST_CASE("WAV File - rejects malformed input", "[media][unit][wav][file]")
  {
    SECTION("empty audio data")
    {
      auto data = ao::test::wav::makeWav({.audioData = {}});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      auto result = file.readContent();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("malformed embedded ID3 tag")
    {
      auto malformedId3 =
        std::vector<std::uint8_t>{'I', 'D', '3', 3, 0, 0,   0,   0,   0,   22,  'T', 'I', 'T', '2', 0, 0,
                                  0,   2,   0,   0, 0, 'A', 'T', 'P', 'E', '1', 0,   0,   0,   100, 0, 0};
      auto data = ao::test::wav::makeWav({
        .extraChunks = {{{.id = {'i', 'd', '3', ' '}, .payload = malformedId3}}},
      });
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      auto result = file.readContent();

      REQUIRE(result);
      CHECK(result->text(TextField::Title).empty());
      CHECK(result->codec() == AudioCodec::Wav);
    }

    SECTION("malformed LIST chunk discards fields parsed before the error")
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
      auto result = file.readContent();

      REQUIRE(result);
      CHECK(result->text(TextField::Title).empty());
      CHECK(result->codec() == AudioCodec::Wav);
    }
  }
} // namespace ao::media::file::wav::test
