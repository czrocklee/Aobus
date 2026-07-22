// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/detail/TrackFieldGridSchema.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("buildTrackFieldGridSchema builds rows from presentable track fields", "[uimodel][unit][library][detail]")
  {
    auto const projection = buildTrackFieldGridSchema();

    CHECK(projection.metadataFields == std::vector{rt::TrackField::Title,
                                                   rt::TrackField::Artist,
                                                   rt::TrackField::Album,
                                                   rt::TrackField::AlbumArtist,
                                                   rt::TrackField::Genre,
                                                   rt::TrackField::Composer,
                                                   rt::TrackField::Conductor,
                                                   rt::TrackField::Ensemble,
                                                   rt::TrackField::Work,
                                                   rt::TrackField::Movement,
                                                   rt::TrackField::Soloist,
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

  TEST_CASE("buildTrackFieldGridSchema filters sections from request flags", "[uimodel][unit][library][detail]")
  {
    auto const metadataOnly =
      buildTrackFieldGridSchema(TrackFieldGridSchemaOptions{.includeMetadata = true, .includeTechnical = false});
    CHECK_FALSE(metadataOnly.metadataFields.empty());
    CHECK_FALSE(metadataOnly.compositeMetadataFields.empty());
    CHECK(metadataOnly.technicalFields.empty());

    auto const technicalOnly =
      buildTrackFieldGridSchema(TrackFieldGridSchemaOptions{.includeMetadata = false, .includeTechnical = true});
    CHECK(technicalOnly.metadataFields.empty());
    CHECK(technicalOnly.compositeMetadataFields.empty());
    CHECK_FALSE(technicalOnly.technicalFields.empty());
  }

  TEST_CASE("formatMetadataHeader summarizes collapsed metadata", "[uimodel][unit][library][detail]")
  {
    CHECK(formatMetadataHeader("Song", "Artist") == "Song — Artist");
    CHECK(formatMetadataHeader("Song", "") == "Song");
    CHECK(formatMetadataHeader("", "Artist") == "Artist");
    CHECK(formatMetadataHeader("", "") == "Metadata");
  }

  TEST_CASE("formatTechnicalHeader summarizes collapsed audio properties", "[uimodel][unit][library][detail]")
  {
    CHECK(formatTechnicalHeader("FLAC", "44.1 kHz", "16-bit") == "FLAC · 44.1 kHz · 16-bit");
    CHECK(formatTechnicalHeader("", "44.1 kHz", "") == "44.1 kHz");
    CHECK(formatTechnicalHeader("", "", "") == "Audio Properties");
  }
} // namespace ao::uimodel::test
