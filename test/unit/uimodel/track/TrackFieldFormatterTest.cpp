// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

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
    REQUIRE(formatDuration(milliseconds{0}).empty());
    REQUIRE(formatDuration(milliseconds{1000}) == "0:01");
    REQUIRE(formatDuration(milliseconds{61000}) == "1:01");
    REQUIRE(formatDuration(milliseconds{3600000}) == "1:0:00"); // 60 minutes
  }

  TEST_CASE("TrackFieldFormatter - uint16 formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatUint16(0).empty());
    REQUIRE(formatUint16(1) == "1");
    REQUIRE(formatUint16(65535) == "65535");
  }

  TEST_CASE("TrackFieldFormatter - filesize formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatFileSize(0).empty());
    REQUIRE(formatFileSize(512) == "0.5 KB");
    REQUIRE(formatFileSize(1024) == "1.0 KB");
    REQUIRE(formatFileSize(1048576) == "1.0 MB");
  }

  TEST_CASE("TrackFieldFormatter - sample rate formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatSampleRate(0).empty());
    REQUIRE(formatSampleRate(44100) == "44100 Hz");
    REQUIRE(formatSampleRate(48000) == "48000 Hz");
    REQUIRE(formatSampleRate(192000) == "192000 Hz");
  }

  TEST_CASE("TrackFieldFormatter - sample rate compact formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatSampleRateCompact(0).empty());
    REQUIRE(formatSampleRateCompact(44100) == "44.1 kHz");
    REQUIRE(formatSampleRateCompact(48000) == "48 kHz");
    REQUIRE(formatSampleRateCompact(192000) == "192 kHz");
  }

  TEST_CASE("TrackFieldFormatter - bitrate formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatBitrate(0).empty());
    REQUIRE(formatBitrate(320000) == "320 kbps");
    REQUIRE(formatBitrate(1411000) == "1411 kbps");
  }

  TEST_CASE("TrackFieldFormatter - channels formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatChannels(0).empty());
    REQUIRE(formatChannels(1) == "Mono");
    REQUIRE(formatChannels(2) == "Stereo");
    REQUIRE(formatChannels(3) == "3 channels");
    REQUIRE(formatChannels(6) == "6 channels");
    REQUIRE(formatChannels(8) == "8 channels");
  }

  TEST_CASE("TrackFieldFormatter - bit depth formatting", "[uimodel][track][formatter]")
  {
    REQUIRE(formatBitDepth(0).empty());
    REQUIRE(formatBitDepth(16) == "16-bit");
    REQUIRE(formatBitDepth(24) == "24-bit");
  }

  TEST_CASE("TrackFieldFormatter - text editing", "[uimodel][track][formatter]")
  {
    auto const editVal = makeTextEditValue(" Test ");
    auto const* str = std::get_if<std::string>(&editVal);
    REQUIRE(str != nullptr);
    REQUIRE(*str == " Test "); // Keeps whitespace for generic text
  }

  TEST_CASE("TrackFieldFormatter - uint16 parsing", "[uimodel][track][formatter]")
  {
    SECTION("Valid numbers")
    {
      auto const res = parseUint16EditValue("  42  ");
      REQUIRE(res.has_value());
      auto const* val = std::get_if<std::uint16_t>(&res.value());
      REQUIRE(val != nullptr);
      REQUIRE(*val == 42);
    }

    SECTION("Empty string maps to 0")
    {
      auto const res = parseUint16EditValue("    ");
      REQUIRE(res.has_value());
      auto const* val = std::get_if<std::uint16_t>(&res.value());
      REQUIRE(val != nullptr);
      REQUIRE(*val == 0);
    }

    SECTION("Invalid numbers return error")
    {
      REQUIRE(!parseUint16EditValue("abc").has_value());
      REQUIRE(!parseUint16EditValue("-1").has_value());
      REQUIRE(!parseUint16EditValue("65536").has_value()); // Out of bounds
    }
  }
} // namespace ao::uimodel::track::test
