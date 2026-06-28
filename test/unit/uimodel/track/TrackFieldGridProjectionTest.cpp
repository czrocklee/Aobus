// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackFieldGridProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::track::test
{
  TEST_CASE("projectTrackFieldGrid builds rows from presentable track fields", "[uimodel][unit][track][field-grid]")
  {
    auto const projection = projectTrackFieldGrid(TrackFieldGridProjectionRequest{});

    CHECK(projection.metadataFields == std::vector{rt::TrackField::Title,
                                                   rt::TrackField::Artist,
                                                   rt::TrackField::Album,
                                                   rt::TrackField::AlbumArtist,
                                                   rt::TrackField::Genre,
                                                   rt::TrackField::Composer,
                                                   rt::TrackField::Work,
                                                   rt::TrackField::Movement,
                                                   rt::TrackField::Year});

    CHECK(projection.compositeMetadataFields ==
          std::vector{TrackFieldGridCompositeFields{
                        .primaryField = rt::TrackField::DiscNumber, .secondaryField = rt::TrackField::DiscTotal},
                      TrackFieldGridCompositeFields{
                        .primaryField = rt::TrackField::TrackNumber, .secondaryField = rt::TrackField::TrackTotal},
                      TrackFieldGridCompositeFields{.primaryField = rt::TrackField::MovementNumber,
                                                    .secondaryField = rt::TrackField::MovementTotal}});

    CHECK(projection.technicalFields == std::vector{rt::TrackField::Duration,
                                                    rt::TrackField::FilePath,
                                                    rt::TrackField::Codec,
                                                    rt::TrackField::SampleRate,
                                                    rt::TrackField::Channels,
                                                    rt::TrackField::BitDepth,
                                                    rt::TrackField::Bitrate,
                                                    rt::TrackField::FileSize,
                                                    rt::TrackField::ModifiedTime});
  }

  TEST_CASE("projectTrackFieldGrid filters sections from request flags", "[uimodel][unit][track][field-grid]")
  {
    auto const metadataOnly =
      projectTrackFieldGrid(TrackFieldGridProjectionRequest{.includeMetadata = true, .includeTechnical = false});
    CHECK_FALSE(metadataOnly.metadataFields.empty());
    CHECK_FALSE(metadataOnly.compositeMetadataFields.empty());
    CHECK(metadataOnly.technicalFields.empty());

    auto const technicalOnly =
      projectTrackFieldGrid(TrackFieldGridProjectionRequest{.includeMetadata = false, .includeTechnical = true});
    CHECK(technicalOnly.metadataFields.empty());
    CHECK(technicalOnly.compositeMetadataFields.empty());
    CHECK_FALSE(technicalOnly.technicalFields.empty());
  }

  TEST_CASE("formatTrackFieldGridMetadataHeader summarizes collapsed metadata", "[uimodel][unit][track][field-grid]")
  {
    CHECK(formatTrackFieldGridMetadataHeader("Song", "Artist") == "Song — Artist");
    CHECK(formatTrackFieldGridMetadataHeader("Song", "") == "Song");
    CHECK(formatTrackFieldGridMetadataHeader("", "Artist") == "Artist");
    CHECK(formatTrackFieldGridMetadataHeader("", "") == "Metadata");
  }

  TEST_CASE("formatTrackFieldGridTechnicalHeader summarizes collapsed audio properties",
            "[uimodel][unit][track][field-grid]")
  {
    CHECK(formatTrackFieldGridTechnicalHeader("FLAC", "44.1 kHz", "16-bit") == "FLAC · 44.1 kHz · 16-bit");
    CHECK(formatTrackFieldGridTechnicalHeader("", "44.1 kHz", "") == "44.1 kHz");
    CHECK(formatTrackFieldGridTechnicalHeader("", "", "") == "Audio Properties");
  }
} // namespace ao::uimodel::track::test
