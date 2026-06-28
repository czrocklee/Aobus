// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    std::vector<rt::TrackField> fieldsFrom(std::vector<TrackPropertiesFormRow> const& rows)
    {
      auto fields = std::vector<rt::TrackField>{};
      fields.reserve(rows.size());

      for (auto const& row : rows)
      {
        fields.push_back(row.field);
      }

      return fields;
    }
  } // namespace

  TEST_CASE("TrackPropertiesFormSpec projects editable metadata rows", "[uimodel][unit][library][property]")
  {
    auto const spec = buildTrackPropertiesFormSpec();

    CHECK(fieldsFrom(spec.metadataRows) == std::vector{rt::TrackField::Title,
                                                       rt::TrackField::Artist,
                                                       rt::TrackField::Album,
                                                       rt::TrackField::AlbumArtist,
                                                       rt::TrackField::Genre,
                                                       rt::TrackField::Composer,
                                                       rt::TrackField::Work,
                                                       rt::TrackField::Movement,
                                                       rt::TrackField::Year,
                                                       rt::TrackField::DiscNumber,
                                                       rt::TrackField::DiscTotal,
                                                       rt::TrackField::TrackNumber,
                                                       rt::TrackField::TrackTotal,
                                                       rt::TrackField::MovementNumber,
                                                       rt::TrackField::MovementTotal});

    REQUIRE(spec.metadataRows.size() > 9U);
    CHECK(spec.metadataRows[0].label == "Title");
    CHECK(spec.metadataRows[0].editorKind == TrackPropertiesFormEditorKind::Text);
    CHECK(spec.metadataRows[8].field == rt::TrackField::Year);
    CHECK(spec.metadataRows[8].label == "Year");
    CHECK(spec.metadataRows[8].editorKind == TrackPropertiesFormEditorKind::Number);
  }

  TEST_CASE("TrackPropertiesFormSpec projects readonly technical property rows", "[uimodel][unit][library][property]")
  {
    auto const spec = buildTrackPropertiesFormSpec();

    CHECK(fieldsFrom(spec.propertyRows) == std::vector{rt::TrackField::Duration,
                                                       rt::TrackField::FilePath,
                                                       rt::TrackField::Codec,
                                                       rt::TrackField::SampleRate,
                                                       rt::TrackField::Channels,
                                                       rt::TrackField::BitDepth,
                                                       rt::TrackField::Bitrate,
                                                       rt::TrackField::FileSize,
                                                       rt::TrackField::ModifiedTime});

    REQUIRE(spec.propertyRows.size() > 3U);
    CHECK(spec.propertyRows[0].label == "Duration");
    CHECK(spec.propertyRows[0].editorKind == TrackPropertiesFormEditorKind::ReadonlyText);
  }
} // namespace ao::uimodel::test
