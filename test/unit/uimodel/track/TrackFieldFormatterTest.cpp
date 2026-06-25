// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/uimodel/track/TrackFieldFormatter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace ao::uimodel::track::test
{
  using namespace ao::library;
  using namespace std::chrono_literals;

  TEST_CASE("TrackFieldFormatter - duration formatting", "[uimodel][track][formatter]")
  {
    using std::chrono::milliseconds;
    CHECK(formatDuration(milliseconds{0}).empty());
    CHECK(formatDuration(std::chrono::seconds{1}) == "0:01");
    CHECK(formatDuration(std::chrono::seconds{61}) == "1:01");
    CHECK(formatDuration(std::chrono::minutes{60}) == "1:0:00"); // 60 minutes
  }

  TEST_CASE("TrackFieldFormatter - uint16 formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatUint16(0).empty());
    CHECK(formatUint16(1) == "1");
    CHECK(formatUint16(65535) == "65535");
  }

  TEST_CASE("TrackFieldFormatter - filesize formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatFileSize(0).empty());
    CHECK(formatFileSize(512) == "0.5 KB");
    CHECK(formatFileSize(1024) == "1.0 KB");
    CHECK(formatFileSize(1048576) == "1.0 MB");
  }

  TEST_CASE("TrackFieldFormatter - sample rate formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatSampleRate(0).empty());
    CHECK(formatSampleRate(44100) == "44100 Hz");
    CHECK(formatSampleRate(48000) == "48000 Hz");
    CHECK(formatSampleRate(192000) == "192000 Hz");
  }

  TEST_CASE("TrackFieldFormatter - sample rate compact formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatSampleRateCompact(0).empty());
    CHECK(formatSampleRateCompact(44100) == "44.1 kHz");
    CHECK(formatSampleRateCompact(48000) == "48 kHz");
    CHECK(formatSampleRateCompact(192000) == "192 kHz");
  }

  TEST_CASE("TrackFieldFormatter - bitrate formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatBitrate(0).empty());
    CHECK(formatBitrate(320000) == "320 kbps");
    CHECK(formatBitrate(1411000) == "1411 kbps");
  }

  TEST_CASE("TrackFieldFormatter - channels formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatChannels(0).empty());
    CHECK(formatChannels(1) == "Mono");
    CHECK(formatChannels(2) == "Stereo");
    CHECK(formatChannels(3) == "3 channels");
    CHECK(formatChannels(6) == "6 channels");
    CHECK(formatChannels(8) == "8 channels");
  }

  TEST_CASE("TrackFieldFormatter - bit depth formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatBitDepth(0).empty());
    CHECK(formatBitDepth(16) == "16-bit");
    CHECK(formatBitDepth(24) == "24-bit");
  }

  TEST_CASE("TrackFieldFormatter - codec formatting", "[uimodel][track][formatter]")
  {
    CHECK(formatCodec(AudioCodec::Unknown).empty());
    CHECK(formatCodec(AudioCodec::Flac) == "FLAC");
    CHECK(formatCodec(AudioCodec::Alac) == "ALAC");
    CHECK(formatCodec(AudioCodec::Aac) == "AAC");
  }

  TEST_CASE("TrackFieldFormatter - text editing", "[uimodel][track][formatter]")
  {
    auto const editVal = makeTextEditValue(" Test ");
    auto const* str = std::get_if<std::string>(&editVal);
    REQUIRE(str != nullptr);
    CHECK(*str == " Test "); // Keeps whitespace for generic text
  }

  TEST_CASE("TrackFieldFormatter - uint16 parsing", "[uimodel][track][formatter]")
  {
    SECTION("Valid numbers")
    {
      auto const res = parseUint16EditValue("  42  ");
      REQUIRE(res.has_value());
      auto const* val = std::get_if<std::uint16_t>(&*res);
      REQUIRE(val != nullptr);
      CHECK(*val == 42);
    }

    SECTION("Empty string maps to 0")
    {
      auto const res = parseUint16EditValue("    ");
      REQUIRE(res.has_value());
      auto const* val = std::get_if<std::uint16_t>(&*res);
      REQUIRE(val != nullptr);
      CHECK(*val == 0);
    }

    SECTION("Invalid numbers return error")
    {
      CHECK(!parseUint16EditValue("abc").has_value());
      CHECK(!parseUint16EditValue("-1").has_value());
      CHECK(!parseUint16EditValue("65536").has_value()); // Out of bounds
    }
  }
} // namespace ao::uimodel::track::test
