// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/PictureType.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::media::file::test
{
  namespace
  {
    class ExtentVisitorSpy final : public Visitor
    {
    public:
      void text(TextField /*field*/, std::string_view /*value*/) override {}
      void number(NumberField /*field*/, std::uint16_t /*value*/) override {}
      void codec(AudioCodec /*value*/) override {}
      void duration(std::chrono::milliseconds duration) override { _duration = duration; }
      void bitrate(Bitrate /*value*/) override {}
      void sampleRate(SampleRate value) override { _sampleRate = value.raw(); }
      void channels(Channels /*value*/) override {}
      void bitDepth(BitDepth /*value*/) override {}
      void picture(PictureType /*type*/, std::span<std::byte const> /*bytes*/) override {}
      void exactFrameCount(std::uint64_t value) override { _optFrames = value; }
      void cueSheet(std::string_view value) override { _sheets.emplace_back(value); }

      std::uint32_t sampleRate() const noexcept { return _sampleRate; }
      std::chrono::milliseconds duration() const noexcept { return _duration; }
      std::optional<std::uint64_t> exactFrames() const noexcept { return _optFrames; }
      std::vector<std::string> const& sheets() const noexcept { return _sheets; }

    private:
      std::uint32_t _sampleRate = 0;
      std::chrono::milliseconds _duration{0};
      std::optional<std::uint64_t> _optFrames{};
      std::vector<std::string> _sheets;
    };

    void appendBlock(std::vector<std::uint8_t>& bytes,
                     std::uint8_t type,
                     bool last,
                     std::span<std::uint8_t const> payload)
    {
      auto const length = static_cast<std::uint32_t>(payload.size());
      bytes.push_back(static_cast<std::uint8_t>(type | (last ? 0x80U : 0U)));
      bytes.push_back(static_cast<std::uint8_t>(length >> 16U));
      bytes.push_back(static_cast<std::uint8_t>(length >> 8U));
      bytes.push_back(static_cast<std::uint8_t>(length));
      bytes.insert(bytes.end(), payload.begin(), payload.end());
    }

    void appendCommentString(std::vector<std::uint8_t>& bytes, std::string_view value)
    {
      ao::test::wav::appendLe32(bytes, static_cast<std::uint32_t>(value.size()));
      bytes.insert(bytes.end(), value.begin(), value.end());
    }

    std::vector<std::uint8_t> flacHeader(std::uint64_t totalFrames, std::optional<std::string_view> optSheet)
    {
      auto bytes = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};
      auto streamInfo = std::vector<std::uint8_t>(34, 0);
      auto const packed = (44100ULL << 44U) | (1ULL << 41U) | (15ULL << 36U) | totalFrames;

      for (std::size_t index = 0; index < 8; ++index)
      {
        streamInfo[10 + index] = static_cast<std::uint8_t>(packed >> ((7 - index) * 8U));
      }

      appendBlock(bytes, 0, false, streamInfo);

      if (optSheet)
      {
        auto comments = std::vector<std::uint8_t>{};
        appendCommentString(comments, "test vendor");
        ao::test::wav::appendLe32(comments, 1);
        appendCommentString(comments, "cUeShEeT=" + std::string{*optSheet});
        appendBlock(bytes, 4, false, comments);
      }

      // Binary CUESHEET is deliberately uninterpreted, not textual CUE input.
      appendBlock(bytes, 5, true, std::vector<std::uint8_t>(396, 0));
      bytes.push_back(0xA0);
      return bytes;
    }
  } // namespace

  TEST_CASE("Media extent - WAV frame counts retain partial milliseconds", "[media][unit][exact-extent]")
  {
    auto const frames = GENERATE(std::uint64_t{1}, std::uint64_t{574}, std::uint64_t{588});
    auto const bytes = ao::test::wav::makeWav(
      {.channels = 2, .sampleRate = 44100, .audioData = std::vector<std::uint8_t>(frames * 4, 0)});
    auto const temp = ao::test::TempFile{bytes, ".wav"};
    auto const file = ao::test::requireValue(File::open(temp.path));
    auto visitor = ExtentVisitorSpy{};
    REQUIRE(file.visit(visitor));
    CHECK(visitor.sampleRate() == 44100);
    CHECK(visitor.exactFrames() == frames);
    CHECK(visitor.duration() == std::chrono::milliseconds{frames == 1 ? 0 : 13});
    CHECK(visitor.sheets().empty());
  }

  TEST_CASE("Media extent - FLAC reports nonzero STREAMINFO frames but not unknown zero", "[media][unit][exact-extent]")
  {
    auto const frames =
      GENERATE(std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{588}, (std::uint64_t{1} << 36U) - 1U);
    auto const temp = ao::test::TempFile{flacHeader(frames, std::nullopt), ".flac"};
    auto const file = ao::test::requireValue(File::open(temp.path));
    auto visitor = ExtentVisitorSpy{};
    REQUIRE(file.visit(visitor));
    CHECK(visitor.sampleRate() == 44100);

    if (frames == 0)
    {
      CHECK_FALSE(visitor.exactFrames());
    }
    else
    {
      CHECK(visitor.exactFrames() == frames);
    }

    CHECK(visitor.sheets().empty());
  }

  TEST_CASE("Media extent - embedded text CUE is copied verbatim while binary CUESHEET stays skipped",
            "[media][unit][exact-extent]")
  {
    auto const sheet = std::string{"FILE \"image.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"};
    auto visitor = ExtentVisitorSpy{};

    {
      auto const temp = ao::test::TempFile{flacHeader(588, sheet), ".flac"};
      auto const file = ao::test::requireValue(File::open(temp.path));
      REQUIRE(file.visit(visitor));
    }

    CHECK(visitor.sheets() == std::vector<std::string>{sheet});
    CHECK(visitor.exactFrames() == 588);
  }

  TEST_CASE("Media extent - truncated or nonaligned WAV data never advertises a carrier extent",
            "[media][unit][exact-extent]")
  {
    auto bytes =
      ao::test::wav::makeWav({.channels = 2, .sampleRate = 44100, .audioData = std::vector<std::uint8_t>(16, 0)});

    SECTION("payload truncated behind an intact data header")
    {
      bytes.pop_back();
    }

    SECTION("data length is not a whole frame")
    {
      // Canonical tiny RIFF: data length DWORD follows the data id at byte 36.
      bytes[40] = 15;
    }

    auto const temp = ao::test::TempFile{bytes, ".wav"};
    auto const file = ao::test::requireValue(File::open(temp.path));
    auto visitor = ExtentVisitorSpy{};
    auto const res = file.visit(visitor);
    REQUIRE_FALSE(res);
    CHECK_FALSE(visitor.exactFrames());
  }
} // namespace ao::media::file::test
