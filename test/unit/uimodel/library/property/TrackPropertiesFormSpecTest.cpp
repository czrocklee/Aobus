// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPropertiesFormSpec - projects editable metadata rows", "[uimodel][unit][property]")
  {
    using E = TrackPropertiesFormEditorKind;
    using F = rt::TrackField;

    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto const expected = std::vector<TrackPropertiesFormRow>{
      {.field = F::Title, .label = "Title", .editorKind = E::Text},
      {.field = F::Artist, .label = "Artist", .editorKind = E::Text},
      {.field = F::Album, .label = "Album", .editorKind = E::Text},
      {.field = F::AlbumArtist, .label = "Album Artist", .editorKind = E::Text},
      {.field = F::Genre, .label = "Genre", .editorKind = E::Text},
      {.field = F::Composer, .label = "Composer", .editorKind = E::Text},
      {.field = F::Conductor, .label = "Conductor", .editorKind = E::Text},
      {.field = F::Ensemble, .label = "Ensemble", .editorKind = E::Text},
      {.field = F::Work, .label = "Work", .editorKind = E::Text},
      {.field = F::Movement, .label = "Movement", .editorKind = E::Text},
      {.field = F::Soloist, .label = "Soloist", .editorKind = E::Text},
      {.field = F::Year, .label = "Year", .editorKind = E::Number},
      {.field = F::DiscNumber, .label = "Disc", .editorKind = E::Number},
      {.field = F::DiscTotal, .label = "Total Discs", .editorKind = E::Number},
      {.field = F::TrackNumber, .label = "Track", .editorKind = E::Number},
      {.field = F::TrackTotal, .label = "Total Tracks", .editorKind = E::Number},
      {.field = F::MovementNumber, .label = "Movement No.", .editorKind = E::Number},
      {.field = F::MovementTotal, .label = "Total Movements", .editorKind = E::Number},
    };

    CHECK(spec.metadataRows == expected);
  }

  TEST_CASE("TrackPropertiesFormSpec - projects readonly technical property rows", "[uimodel][unit][property]")
  {
    using E = TrackPropertiesFormEditorKind;
    using F = rt::TrackField;

    auto const spec = buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
    auto const expected = std::vector<TrackPropertiesFormRow>{
      {.field = F::Duration, .label = "Duration", .editorKind = E::ReadonlyText},
      {.field = F::FilePath, .label = "File Path", .editorKind = E::ReadonlyText},
      {.field = F::Codec, .label = "Codec", .editorKind = E::ReadonlyText},
      {.field = F::SampleRate, .label = "Sample Rate", .editorKind = E::ReadonlyText},
      {.field = F::Channels, .label = "Channels", .editorKind = E::ReadonlyText},
      {.field = F::BitDepth, .label = "Bit Depth", .editorKind = E::ReadonlyText},
      {.field = F::Bitrate, .label = "Bitrate", .editorKind = E::ReadonlyText},
      {.field = F::FileSize, .label = "File Size", .editorKind = E::ReadonlyText},
      {.field = F::ModifiedTime, .label = "Modified", .editorKind = E::ReadonlyText},
    };

    CHECK(spec.propertyRows == expected);
  }

  TEST_CASE("TrackPropertiesFormSpec - owns labels from a temporary locale catalog",
            "[uimodel][unit][property][localization]")
  {
    auto const spec = buildTrackPropertiesFormSpec(ao::test::messageCatalog("de-DE"));

    REQUIRE_FALSE(spec.metadataRows.empty());
    CHECK(spec.metadataRows.front().label == "Titel");
  }
} // namespace ao::uimodel::test
