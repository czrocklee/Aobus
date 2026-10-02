// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/field/TrackFieldFormatter.h>

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/FileTimestamp.h>
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
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// NOLINTNEXTLINE(misc-include-cleaner) -- each standard library defines the macro in a different header.
#if __cpp_lib_chrono >= 201907L
#include <stdexcept>
#else
#include <ctime>
#endif

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
#if __cpp_lib_chrono >= 201907L
      try
      {
        return std::format("{:%Y-%m-%d %H:%M}", std::chrono::zoned_time{std::chrono::current_zone(), time});
      }
      catch (std::runtime_error const&)
      {
        return std::format("{:%Y-%m-%d %H:%M}", time);
      }
#else
      // The C library reads the host zone independently of the date library
      // that substitutes for the missing standard time-zone database.
      auto const seconds = static_cast<std::time_t>(time.time_since_epoch().count());
      auto local = std::tm{};
      REQUIRE(::localtime_r(&seconds, &local) != nullptr);
      return std::format("{:04}-{:02}-{:02} {:02}:{:02}",
                         local.tm_year + 1900,
                         local.tm_mon + 1,
                         local.tm_mday,
                         local.tm_hour,
                         local.tm_min);
#endif
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

  TEST_CASE("TrackFieldFormatter - absent modification time renders empty while epoch zero renders its date",
            "[uimodel][unit][field][formatter]")
  {
    CHECK(formatTime(std::nullopt).empty());

    // A stored epoch-zero instant is present, not absent: 1970-01-01 00:00 UTC.
    auto const epochInstant = FileTimestamp{};
    CHECK(formatTime(epochInstant) == expectedLocalTime(std::chrono::sys_seconds{}));
  }

  TEST_CASE("TrackFieldFormatter - seconds outside the display calendar do not overflow or wrap years",
            "[uimodel][unit][field][formatter]")
  {
    CHECK(formatTime(FileTimestamp{.seconds = std::numeric_limits<std::int64_t>::min()}).empty());
    CHECK(formatTime(FileTimestamp{.seconds = std::numeric_limits<std::int64_t>::max()}).empty());
  }

  TEST_CASE("TrackFieldFormatter - Unix modification seconds render the local date",
            "[uimodel][unit][field][formatter]")
  {
    // 2024-07-01 12:00:00 UTC stored as plain Unix seconds; noon UTC in mid-2024
    // keeps the rendered year the same across local zone offsets.
    auto const mtime = FileTimestamp{.seconds = 1719835200};
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

    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::July / std::chrono::day{1}} + std::chrono::hours{12};
    CHECK(text == expectedLocalTime(modificationTime));
  }

  TEST_CASE("TrackFieldFormatter - negative Unix seconds render pre-epoch dates", "[uimodel][unit][field][formatter]")
  {
    // 1938-07-01 12:00:00 UTC predates the Unix epoch.
    auto const mtime = FileTimestamp{.seconds = -994161600};
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{1938} / std::chrono::July / std::chrono::day{1}} + std::chrono::hours{12};
    CHECK(formatTime(mtime) == expectedLocalTime(modificationTime));
  }

  TEST_CASE("TrackFieldFormatter - Unix seconds beyond the nanosecond range render far-future dates",
            "[uimodel][unit][field][formatter]")
  {
    // 2459-07-01 12:00:00 UTC lies beyond 2262, the signed 64-bit nanosecond
    // horizon; explicit Unix seconds keep the instant representable.
    auto const mtime = FileTimestamp{.seconds = 15447067200};
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2459} / std::chrono::July / std::chrono::day{1}} + std::chrono::hours{12};
    CHECK(formatTime(mtime) == expectedLocalTime(modificationTime));
  }

  TEST_CASE("TrackFieldFormatter - nanosecond fraction stays in the payload without shifting the minute",
            "[uimodel][unit][field][formatter]")
  {
    // 2024-07-01 12:00:59.5 UTC: the fraction must not round the minute up.
    auto const mtime = FileTimestamp{.seconds = 1719835259, .nanoseconds = 500000000};
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::July / std::chrono::day{1}} +
      std::chrono::hours{12} + std::chrono::seconds{59};
    CHECK(formatTime(mtime) == expectedLocalTime(modificationTime));

    // 1938-07-01 11:59:59.5 UTC: a negative second floors to 11:59, not 12:00.
    auto const preEpochMtime = FileTimestamp{.seconds = -994161601, .nanoseconds = 500000000};
    auto const preEpoch = std::chrono::sys_days{std::chrono::year{1938} / std::chrono::July / std::chrono::day{1}} +
                          std::chrono::hours{11} + std::chrono::minutes{59} + std::chrono::seconds{59};
    CHECK(formatTime(preEpochMtime) == expectedLocalTime(preEpoch));

    // The fraction rides in the raw-value payload; equality compares both parts.
    auto const withFraction = TrackFieldRawValue{std::in_place_type<FileTimestamp>, mtime};
    CHECK(withFraction != TrackFieldRawValue{std::in_place_type<FileTimestamp>, FileTimestamp{.seconds = 1719835259}});
    CHECK(withFraction == TrackFieldRawValue{std::in_place_type<FileTimestamp>, mtime});
  }

  TEST_CASE("TrackFieldFormatter - scanned manifest modification time renders the file's local date",
            "[uimodel][integration][field][formatter]")
  {
    auto libraryFixture = rt::test::MusicLibraryFixture{};
    auto const audioPath = libraryFixture.root() / "song.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), audioPath);
    // A fractional instant just before a minute boundary keeps the nanosecond
    // fraction in the stored manifest.
    auto const modificationTime =
      std::chrono::sys_days{std::chrono::year{2024} / std::chrono::July / std::chrono::day{1}} +
      std::chrono::hours{12} + std::chrono::seconds{59} + std::chrono::milliseconds{500};
    auto const fileTime = ao::test::fileTimeFromSystemTime(modificationTime);
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

    // The manifest stores the exact Unix instant: 2024-07-01T12:00:59.5Z.
    auto const optMtime = optManifest->mtime();
    REQUIRE(optMtime);
    CHECK(*optMtime == FileTimestamp{.seconds = 1719835259, .nanoseconds = 500000000});

    CHECK(formatTime(optMtime) == expectedLocalTime(std::chrono::floor<std::chrono::seconds>(modificationTime)));
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
    auto const modifiedTime = FileTimestamp{.seconds = 1719835259, .nanoseconds = 500000000};
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::ModifiedTime,
                                   Raw{std::in_place_type<FileTimestamp>, modifiedTime}) == formatTime(modifiedTime));
    // Only the FileTimestamp alternative formats a modification time; the raw
    // uint64 count no longer carries one.
    CHECK(formatTrackFieldRawValue(catalog,
                                   TrackField::ModifiedTime,
                                   Raw{std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(1719835259)})
            .empty());
    CHECK(formatTrackFieldRawValue(catalog, TrackField::ModifiedTime, Raw{std::monostate{}}).empty());
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
