// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ao::uimodel::test
{
  using namespace ao::library;
  using namespace ao::rt;
  using namespace std::chrono_literals;

  namespace
  {
    TrackDetailSnapshot makeTrackDetailSnapshot()
    {
      return TrackDetailSnapshot{};
    }
  } // namespace

  TEST_CASE("TrackFieldFormatter - duration formatting", "[uimodel][unit][field][formatter]")
  {
    using std::chrono::milliseconds;
    CHECK(formatDuration(milliseconds{0}).empty());
    CHECK(formatDuration(std::chrono::seconds{1}) == "0:01");
    CHECK(formatDuration(std::chrono::seconds{61}) == "1:01");
    CHECK(formatDuration(std::chrono::seconds{225}) == "3:45");
    CHECK(formatDuration(std::chrono::minutes{60}) == "1:00:00");
    CHECK(formatDuration(std::chrono::seconds{3723}) == "1:02:03");
  }

  TEST_CASE("TrackFieldFormatter - uint16 formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatUint16(0).empty());
    CHECK(formatUint16(1) == "1");
    CHECK(formatUint16(65535) == "65535");
  }

  TEST_CASE("TrackFieldFormatter - filesize formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatFileSize(0).empty());
    CHECK(formatFileSize(512) == "0.5 KB");
    CHECK(formatFileSize(1024) == "1.0 KB");
    CHECK(formatFileSize(1048576) == "1.0 MB");
    CHECK(formatFileSize(5242880) == "5.0 MB");
  }

  TEST_CASE("TrackFieldFormatter - sample rate formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatSampleRate(0).empty());
    CHECK(formatSampleRate(44100) == "44100 Hz");
    CHECK(formatSampleRate(48000) == "48000 Hz");
    CHECK(formatSampleRate(192000) == "192000 Hz");
  }

  TEST_CASE("TrackFieldFormatter - sample rate compact formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatSampleRateCompact(0).empty());
    CHECK(formatSampleRateCompact(44100) == "44.1 kHz");
    CHECK(formatSampleRateCompact(48000) == "48 kHz");
    CHECK(formatSampleRateCompact(192000) == "192 kHz");
  }

  TEST_CASE("TrackFieldFormatter - bitrate formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatBitrate(0).empty());
    CHECK(formatBitrate(1000) == "1 kbps");
    CHECK(formatBitrate(1999) == "1 kbps");
    CHECK(formatBitrate(320000) == "320 kbps");
    CHECK(formatBitrate(1411000) == "1411 kbps");
  }

  TEST_CASE("TrackFieldFormatter - channels formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatChannels(0).empty());
    CHECK(formatChannels(1) == "Mono");
    CHECK(formatChannels(2) == "Stereo");
    CHECK(formatChannels(3) == "3 channels");
    CHECK(formatChannels(6) == "6 channels");
    CHECK(formatChannels(8) == "8 channels");
  }

  TEST_CASE("TrackFieldFormatter - bit depth formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatBitDepth(0).empty());
    CHECK(formatBitDepth(16) == "16-bit");
    CHECK(formatBitDepth(24) == "24-bit");
  }

  TEST_CASE("TrackFieldFormatter - codec formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatCodec(AudioCodec::Unknown).empty());
    CHECK(formatCodec(AudioCodec::Flac) == "FLAC");
    CHECK(formatCodec(AudioCodec::Alac) == "ALAC");
    CHECK(formatCodec(AudioCodec::Aac) == "AAC");
  }

  TEST_CASE("TrackFieldFormatter - synthetic row text formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatDisplayTrackNumber(0, 0, 0).empty());
    CHECK(formatDisplayTrackNumber(0, 1, 7) == "7");
    CHECK(formatDisplayTrackNumber(2, 3, 7) == "2-7");
    CHECK(formatDisplayTrackNumber(0, 3, 7) == "7");

    CHECK(formatTechnicalSummary(AudioCodec::Flac, 44100, 16) == "FLAC \u00b7 44.1 kHz \u00b7 16-bit");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 48000, 24) == "48 kHz \u00b7 24-bit");
    CHECK(formatTechnicalSummary(AudioCodec::Flac, 0, 0) == "FLAC");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 48000, 0) == "48 kHz");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 0, 24) == "24-bit");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 0, 0).empty());
  }

  TEST_CASE("formatTrackFieldRawValue formats raw values by field policy", "[uimodel][unit][field][formatter]")
  {
    using Raw = TrackFieldRawValue;

    CHECK(formatTrackFieldRawValue(TrackField::Title, Raw{std::in_place_type<std::string>, "Hello"}) == "Hello");
    CHECK(formatTrackFieldRawValue(TrackField::Title, Raw{std::monostate{}}).empty());
    CHECK(formatTrackFieldRawValue(
            TrackField::Year, Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2024)}) == "2024");
    CHECK(formatTrackFieldRawValue(TrackField::Year, Raw{std::in_place_type<std::string>, "not a number"}).empty());
    CHECK(formatTrackFieldRawValue(
            TrackField::Duration, Raw{std::in_place_type<TrackFieldDuration>, TrackFieldDuration{225000}}) == "3:45");
    CHECK(formatTrackFieldRawValue(
            TrackField::SampleRate, Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(44100)}) ==
          "44100 Hz");
    CHECK(formatTrackFieldRawValue(
            TrackField::Channels, Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(2)}) == "Stereo");
    CHECK(formatTrackFieldRawValue(
            TrackField::BitDepth, Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(24)}) == "24-bit");
    CHECK(formatTrackFieldRawValue(
            TrackField::Bitrate, Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(320000)}) ==
          "320 kbps");
    CHECK(formatTrackFieldRawValue(
            TrackField::FileSize, Raw{std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(1048576)}) ==
          "1.0 MB");
    CHECK(formatTrackFieldRawValue(TrackField::Quality, Raw{std::in_place_type<std::string>, "anything"}).empty());
  }

  TEST_CASE("displayTextForTrackField resolves aggregate display text", "[uimodel][unit][field][formatter]")
  {
    SECTION("mixed aggregate returns caller-provided mixed text")
    {
      auto snap = makeTrackDetailSnapshot();
      trackFieldArrayAt(snap.fields, TrackField::Title).mixed = true;

      auto const result = displayTextForTrackField(TrackField::Title, snap, "<<<mixed>>>", true);

      CHECK(result == "<<<mixed>>>");
    }

    SECTION("unset technical field returns Unknown only when requested")
    {
      auto snap = makeTrackDetailSnapshot();
      auto const& def = *trackFieldDefinition(TrackField::Codec);
      REQUIRE(def.category == TrackFieldCategory::Technical);

      CHECK(displayTextForTrackField(TrackField::Codec, snap, kMultipleTrackValuesText, true) == "Unknown");
      CHECK(displayTextForTrackField(TrackField::Codec, snap, kMultipleTrackValuesText, false).empty());
    }

    SECTION("unset non-technical field returns empty text")
    {
      auto snap = makeTrackDetailSnapshot();

      CHECK(displayTextForTrackField(TrackField::Title, snap, kMultipleTrackValuesText, true).empty());
    }

    SECTION("populated aggregate uses raw field formatter policy")
    {
      auto snap = makeTrackDetailSnapshot();
      trackFieldArrayAt(snap.fields, TrackField::Title).optValue = std::string{"Hello"};
      trackFieldArrayAt(snap.fields, TrackField::Year).optValue = std::uint16_t{2024};
      trackFieldArrayAt(snap.fields, TrackField::Quality).optValue = std::string{"anything"};

      CHECK(displayTextForTrackField(TrackField::Title, snap, kMultipleTrackValuesText, true) == "Hello");
      CHECK(displayTextForTrackField(TrackField::Year, snap, kMultipleTrackValuesText, true) == "2024");
      CHECK(displayTextForTrackField(TrackField::Quality, snap, kMultipleTrackValuesText, true).empty());
    }
  }
} // namespace ao::uimodel::test
