// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/field/TrackRowField.h>

#include "test/MessageCatalogTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::uimodel::test
{
  TEST_CASE("TrackRowField - file path spelling is explicit and an absent title stays empty")
  {
    auto const path = std::filesystem::path{u8"Musik"} / u8"Fünf.flac";
    auto const row = rt::TrackRow{.optUriPath = path};
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(formatTrackRowField(catalog, row, rt::TrackField::FilePath, TrackRowPathFormat::Native) ==
          utility::pathToUtf8(path));
    CHECK(formatTrackRowField(catalog, row, rt::TrackField::FilePath, TrackRowPathFormat::Generic) ==
          "Musik/Fünf.flac");
    CHECK(formatTrackRowField(catalog, row, rt::TrackField::Title, TrackRowPathFormat::Native).empty());
  }

  TEST_CASE("TrackRowField - technical summary and sample rate use the row's technical metadata")
  {
    auto const row = rt::TrackRow{.sampleRate = 96000, .bitDepth = 24, .codec = AudioCodec::Flac, .bitrate = 800000};
    auto const& catalog = ao::test::englishMessageCatalog();

    CHECK(formatTrackRowField(catalog, row, rt::TrackField::SampleRate, TrackRowPathFormat::Native) == "96000 Hz");
    CHECK(formatTrackRowField(catalog, row, rt::TrackField::TechnicalSummary, TrackRowPathFormat::Native) ==
          "FLAC · 96 kHz · 24-bit · 800 kbps");
  }
} // namespace ao::uimodel::test
