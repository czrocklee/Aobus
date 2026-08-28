// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnDefaults.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/rt/TrackField.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("defaultTrackFieldColumnWidth returns presentation widths by field role",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(trackColumnDefaults(rt::TrackField::Artist).width == 150);
    CHECK(trackColumnDefaults(rt::TrackField::Album).width == 200);
    CHECK(trackColumnDefaults(rt::TrackField::TrackNumber).width == 72);
    CHECK(trackColumnDefaults(rt::TrackField::Duration).width == 84);
    CHECK(trackColumnDefaults(rt::TrackField::Year).width == 80);
    CHECK(trackColumnDefaults(rt::TrackField::AlbumArtist).width == 180);
    CHECK(trackColumnDefaults(rt::TrackField::TechnicalSummary).width == 180);
    CHECK(trackColumnDefaults(rt::TrackField::FilePath).width == 300);
  }

  TEST_CASE("trackFieldColumnSizing classifies text columns as flexible", "[uimodel][unit][library][presentation]")
  {
    CHECK(trackColumnDefaults(rt::TrackField::Title).sizing == TrackColumnSizing::Flexible);
    CHECK(trackColumnDefaults(rt::TrackField::Artist).sizing == TrackColumnSizing::Flexible);
    CHECK(trackColumnDefaults(rt::TrackField::Album).sizing == TrackColumnSizing::Flexible);
    CHECK(trackColumnDefaults(rt::TrackField::Tags).sizing == TrackColumnSizing::Flexible);
    CHECK(trackColumnDefaults(rt::TrackField::FilePath).sizing == TrackColumnSizing::Flexible);

    CHECK(trackColumnDefaults(rt::TrackField::Duration).sizing == TrackColumnSizing::Fixed);
    CHECK(trackColumnDefaults(rt::TrackField::Year).sizing == TrackColumnSizing::Fixed);
    CHECK(trackColumnDefaults(rt::TrackField::Bitrate).sizing == TrackColumnSizing::Fixed);
    CHECK(trackColumnDefaults(rt::TrackField::TechnicalSummary).sizing == TrackColumnSizing::Fixed);
  }

  TEST_CASE("trackFieldColumnAlignment classifies bounded scalar values as end aligned",
            "[uimodel][unit][library][presentation]")
  {
    for (auto const field : {rt::TrackField::Year,
                             rt::TrackField::DiscNumber,
                             rt::TrackField::DiscTotal,
                             rt::TrackField::TrackNumber,
                             rt::TrackField::TrackTotal,
                             rt::TrackField::MovementNumber,
                             rt::TrackField::MovementTotal,
                             rt::TrackField::Duration,
                             rt::TrackField::SampleRate,
                             rt::TrackField::Channels,
                             rt::TrackField::BitDepth,
                             rt::TrackField::Bitrate,
                             rt::TrackField::FileSize,
                             rt::TrackField::ModifiedTime,
                             rt::TrackField::DisplayTrackNumber})
    {
      CHECK(trackColumnDefaults(field).alignment == TrackColumnAlignment::End);
    }

    for (auto const field : {rt::TrackField::Title,
                             rt::TrackField::Artist,
                             rt::TrackField::Tags,
                             rt::TrackField::FilePath,
                             rt::TrackField::Codec,
                             rt::TrackField::TechnicalSummary,
                             rt::TrackField::Quality})
    {
      CHECK(trackColumnDefaults(field).alignment == TrackColumnAlignment::Start);
    }
  }

  TEST_CASE("minimumTrackFieldColumnWidth keeps fixed minimums below default widths",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(trackColumnDefaults(rt::TrackField::Title).minimumWidth == 72);
    CHECK(trackColumnDefaults(rt::TrackField::Duration).minimumWidth == 40);
    CHECK(trackColumnDefaults(rt::TrackField::Duration).minimumWidth <
          trackColumnDefaults(rt::TrackField::Duration).width);
    CHECK(trackColumnDefaults(rt::TrackField::Year).minimumWidth < trackColumnDefaults(rt::TrackField::Year).width);
  }

  TEST_CASE("defaultTrackFieldColumnWeight favors title over secondary text fields",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(trackColumnDefaults(rt::TrackField::Title).weight > trackColumnDefaults(rt::TrackField::Artist).weight);
    CHECK(trackColumnDefaults(rt::TrackField::Artist).weight == trackColumnDefaults(rt::TrackField::Album).weight);
    CHECK(trackColumnDefaults(rt::TrackField::Tags).weight > trackColumnDefaults(rt::TrackField::Genre).weight);
  }

  TEST_CASE("trackFieldColumnTitle returns runtime field labels", "[uimodel][unit][library][presentation]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(trackFieldColumnTitle(textCatalog, rt::TrackField::Title) == "Title");
    CHECK(trackFieldColumnTitle(textCatalog, rt::TrackField::Artist) == "Artist");
    CHECK(trackFieldColumnTitle(textCatalog, rt::TrackField::Duration) == "Duration");
  }

  TEST_CASE("presentable runtime fields have default presentation policy", "[uimodel][unit][library][presentation]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();

    for (auto const& rtDef : rt::trackFieldDefinitions())
    {
      if (!rtDef.presentable)
      {
        continue;
      }

      INFO("Field " << rtDef.id << " must have a column title and width");
      CHECK_FALSE(trackFieldColumnTitle(textCatalog, rtDef.field).empty());
      CHECK(trackColumnDefaults(rtDef.field).width > 0);
    }
  }
} // namespace ao::uimodel::test
