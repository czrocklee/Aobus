// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/field/TrackFieldFormatter.h>

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <runtime/library/ScanApplyOperation.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ratio>
#include <stdexcept>
#include <string>
#include <string_view>
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

    std::string expectedLocalTime(std::chrono::sys_seconds const time)
    {
      try
      {
        return std::format("{:%Y-%m-%d %H:%M}", std::chrono::zoned_time{std::chrono::current_zone(), time});
      }
      catch (std::runtime_error const&)
      {
        return std::format("{:%Y-%m-%d %H:%M}", time);
      }
    }

    bool isDigits(std::string_view const text)
    {
      return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
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

  TEST_CASE("TrackFieldFormatter - modified time formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatTime(0).empty());

    // The manifest count uses the file-clock epoch, not the Unix epoch. Noon UTC
    // in mid-2024 keeps the rendered year the same across local zone offsets.
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::July / std::chrono::day{1}} + std::chrono::hours{12};
    auto const fileTime = std::chrono::clock_cast<std::chrono::file_clock>(modificationTime);
    auto const mtime =
      std::chrono::duration_cast<std::chrono::duration<std::uint64_t, std::nano>>(fileTime.time_since_epoch()).count();

    auto const text = formatTime(mtime);

    REQUIRE(text.size() == 16);
    CHECK(text.substr(0, 4) == "2024");
    CHECK(text[4] == '-');
    CHECK(text[7] == '-');
    CHECK(text[10] == ' ');
    CHECK(text[13] == ':');

    auto const month = text.substr(5, 2);
    auto const day = text.substr(8, 2);
    auto const hour = text.substr(11, 2);
    auto const minute = text.substr(14, 2);

    CHECK(isDigits(month));
    CHECK(isDigits(day));
    CHECK(isDigits(hour));
    CHECK(isDigits(minute));
    CHECK((month >= "01" && month <= "12"));
    CHECK((day >= "01" && day <= "31"));
    CHECK(hour <= "23");
    CHECK(minute <= "59");

    CHECK(text == expectedLocalTime(modificationTime));
  }

  TEST_CASE("TrackFieldFormatter - wrapped file-clock nanoseconds preserve dates before 1970",
            "[uimodel][unit][field][formatter]")
  {
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{1938} / std::chrono::July / std::chrono::day{1}} + std::chrono::hours{12};
    auto const fileTime = std::chrono::clock_cast<std::chrono::file_clock>(modificationTime);
    auto const mtime =
      std::chrono::duration_cast<std::chrono::duration<std::uint64_t, std::nano>>(fileTime.time_since_epoch()).count();
    CHECK(formatTime(mtime) == expectedLocalTime(modificationTime));
  }

  TEST_CASE("TrackFieldFormatter - file-clock epoch conversion does not overflow Unix nanoseconds",
            "[uimodel][unit][field][formatter]")
  {
    // This count fits signed file-clock nanoseconds, but its Unix-epoch image
    // need not: on libstdc++ this is a legitimate date beyond 2262.
    constexpr std::uint64_t kMtime = 9000000000000000000;
    auto const fileTime = std::chrono::file_time<std::chrono::seconds>{std::chrono::seconds{9000000000}};
    auto const modificationTime = std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
    CHECK(formatTime(kMtime) == expectedLocalTime(modificationTime));
  }

  TEST_CASE("TrackFieldFormatter - scanned manifest modification time renders the file's local date",
            "[uimodel][integration][field][formatter]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto const audioPath = libraryFixture.root() / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), audioPath);
    // A fractional instant just before a minute boundary also detects truncation
    // toward zero of a negative file-clock count instead of flooring it.
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::July / std::chrono::day{1}} +
      std::chrono::hours{12} + std::chrono::seconds{59} + std::chrono::milliseconds{500};
    auto const fileTime =
      std::filesystem::file_time_type{std::chrono::clock_cast<std::chrono::file_clock>(modificationTime)};
    std::filesystem::last_write_time(audioPath, fileTime);
    INFO("Stamped file time: " << ao::test::formatFileTime(fileTime));
    REQUIRE(std::filesystem::last_write_time(audioPath) == fileTime);

    auto planRes = rt::LibraryScan{libraryFixture.library()}.buildPlan();
    REQUIRE(planRes);
    REQUIRE(planRes->size() == 1);
    auto applyRes = rt::ScanApplyOperation{libraryFixture.library(), std::move(*planRes), {}, {}}.run();
    REQUIRE(applyRes);
    REQUIRE(applyRes->insertedIds.size() == 1);

    auto transaction = libraryFixture.library().readTransaction();
    auto const optManifest = libraryFixture.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    REQUIRE(optManifest->status() == FileStatus::Available);
    CHECK(formatTime(optManifest->mtime()) ==
          expectedLocalTime(std::chrono::floor<std::chrono::seconds>(modificationTime)));
  }

  TEST_CASE("TrackFieldFormatter - sample rate formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatSampleRate(0).empty());
    CHECK(formatSampleRate(44100) == "44100 Hz");
    CHECK(formatSampleRate(48000) == "48000 Hz");
    CHECK(formatSampleRate(192000) == "192000 Hz");
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
    auto const& catalog = ao::test::englishMessageCatalog();
    CHECK(formatChannels(catalog, 0).empty());
    CHECK(formatChannels(catalog, 1) == "Mono");
    CHECK(formatChannels(catalog, 2) == "Stereo");
    CHECK(formatChannels(catalog, 3) == "3 channels");
    CHECK(formatChannels(catalog, 6) == "6 channels");
    CHECK(formatChannels(catalog, 8) == "8 channels");
  }

  TEST_CASE("TrackFieldFormatter - lexical output follows the injected locale", "[uimodel][unit][field][localization]")
  {
    auto const catalog = ao::test::messageCatalog("de-AT");
    auto snap = makeTrackDetailSnapshot();

    CHECK(formatChannels(catalog, 1) == "Mono");
    CHECK(formatChannels(catalog, 2) == "Stereo");
    CHECK(formatChannels(catalog, 6) == "6 Kanäle");
    CHECK(formatTrackFieldDisplayText(catalog, TrackField::Codec, snap, "unused", true) == "Unbekannt");
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
    CHECK(formatCodec(AudioCodec::Wav) == "WAV");
    CHECK(formatCodec(AudioCodec::Aac) == "AAC");
  }

  TEST_CASE("TrackFieldFormatter - display track number formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatDisplayTrackNumber(0, 0, 0).empty());
    CHECK(formatDisplayTrackNumber(0, 1, 7) == "7");
    CHECK(formatDisplayTrackNumber(2, 3, 7) == "2-7");
    CHECK(formatDisplayTrackNumber(0, 3, 7) == "7");
  }

  TEST_CASE("TrackFieldFormatter - technical summary formatting", "[uimodel][unit][field][formatter]")
  {
    CHECK(formatTechnicalSummary(AudioCodec::Flac, 44100, 16, 0) == "FLAC \u00b7 44.1 kHz \u00b7 16-bit");
    CHECK(formatTechnicalSummary(AudioCodec::Wav, 96000, 24, 4608000) ==
          "WAV \u00b7 96 kHz \u00b7 24-bit \u00b7 4608 kbps");
    CHECK(formatTechnicalSummary(AudioCodec::Flac, 44100, 16, 900000) ==
          "FLAC \u00b7 44.1 kHz \u00b7 16-bit \u00b7 900 kbps");
    CHECK(formatTechnicalSummary(AudioCodec::Mp3, 44100, 0, 320000) == "MP3 \u00b7 44.1 kHz \u00b7 320 kbps");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 48000, 24, 0) == "48 kHz \u00b7 24-bit");
    CHECK(formatTechnicalSummary(AudioCodec::Flac, 0, 0, 0) == "FLAC");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 48000, 0, 0) == "48 kHz");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 0, 24, 0) == "24-bit");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 0, 0, 128000) == "128 kbps");
    CHECK(formatTechnicalSummary(AudioCodec::Unknown, 0, 0, 0).empty());
  }

  TEST_CASE("formatTrackFieldRawValue formats raw values by field policy", "[uimodel][unit][field][formatter]")
  {
    using Raw = TrackFieldRawValue;
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(formatTrackFieldRawValue(catalog, TrackField::Title, Raw{std::in_place_type<std::string>, "Hello"}) ==
          "Hello");
    CHECK(formatTrackFieldRawValue(catalog, TrackField::Title, Raw{std::monostate{}}).empty());
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::Year,
                                   Raw{std::in_place_type<std::uint16_t>, static_cast<std::uint16_t>(2024)}) == "2024");
    CHECK(formatTrackFieldRawValue(catalog, TrackField::Year, Raw{std::in_place_type<std::string>, "not a number"})
            .empty());
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::Duration,
                                   Raw{std::in_place_type<TrackFieldDuration>, TrackFieldDuration{225000}}) == "3:45");
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::SampleRate,
                                   Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(44100)}) ==
          "44100 Hz");
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::Channels,
                                   Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(2)}) == "Stereo");
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::BitDepth,
                                   Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(24)}) == "24-bit");
    CHECK(formatTrackFieldRawValue(
            catalog, TrackField::Bitrate, Raw{std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(320000)}) ==
          "320 kbps");
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::FileSize,
                                   Raw{std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(1048576)}) ==
          "1.0 MB");
    CHECK(
      formatTrackFieldRawValue(catalog, TrackField::Quality, Raw{std::in_place_type<std::string>, "anything"}).empty());
  }

  TEST_CASE("formatTrackFieldDisplayText resolves aggregate display text", "[uimodel][unit][field][formatter]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();

    SECTION("mixed aggregate returns caller-provided mixed text")
    {
      auto snap = makeTrackDetailSnapshot();
      trackFieldArrayAt(snap.fields, TrackField::Title).mixed = true;

      auto const result = formatTrackFieldDisplayText(catalog, TrackField::Title, snap, "<<<mixed>>>", true);

      CHECK(result == "<<<mixed>>>");
    }

    SECTION("unset technical field returns Unknown only when requested")
    {
      auto snap = makeTrackDetailSnapshot();
      auto const& def = *trackFieldDefinition(TrackField::Codec);
      REQUIRE(def.category == TrackFieldCategory::Technical);

      CHECK(formatTrackFieldDisplayText(catalog, TrackField::Codec, snap, "unused", true) == "Unknown");
      CHECK(formatTrackFieldDisplayText(catalog, TrackField::Codec, snap, "unused", false).empty());
    }

    SECTION("unset non-technical field returns empty text")
    {
      auto snap = makeTrackDetailSnapshot();

      CHECK(formatTrackFieldDisplayText(catalog, TrackField::Title, snap, "unused", true).empty());
    }

    SECTION("populated aggregate uses raw field formatter policy")
    {
      auto snap = makeTrackDetailSnapshot();
      trackFieldArrayAt(snap.fields, TrackField::Title).optValue = std::string{"Hello"};
      trackFieldArrayAt(snap.fields, TrackField::Year).optValue = std::uint16_t{2024};
      trackFieldArrayAt(snap.fields, TrackField::Quality).optValue = std::string{"anything"};

      CHECK(formatTrackFieldDisplayText(catalog, TrackField::Title, snap, "unused", true) == "Hello");
      CHECK(formatTrackFieldDisplayText(catalog, TrackField::Year, snap, "unused", true) == "2024");
      CHECK(formatTrackFieldDisplayText(catalog, TrackField::Quality, snap, "unused", true).empty());
    }
  }
} // namespace ao::uimodel::test
