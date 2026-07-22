// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("defaultTrackFieldColumnWidth returns presentation widths by field role",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Artist) == 150);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Album) == 200);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::TrackNumber) == 72);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Duration) == 84);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Year) == 80);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::AlbumArtist) == 180);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::TechnicalSummary) == 180);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::FilePath) == 300);
  }

  TEST_CASE("trackFieldColumnSizing classifies text columns as flexible", "[uimodel][unit][library][presentation]")
  {
    CHECK(trackFieldColumnSizing(rt::TrackField::Title) == TrackColumnSizing::Flexible);
    CHECK(trackFieldColumnSizing(rt::TrackField::Artist) == TrackColumnSizing::Flexible);
    CHECK(trackFieldColumnSizing(rt::TrackField::Album) == TrackColumnSizing::Flexible);
    CHECK(trackFieldColumnSizing(rt::TrackField::Tags) == TrackColumnSizing::Flexible);
    CHECK(trackFieldColumnSizing(rt::TrackField::FilePath) == TrackColumnSizing::Flexible);

    CHECK(trackFieldColumnSizing(rt::TrackField::Duration) == TrackColumnSizing::Fixed);
    CHECK(trackFieldColumnSizing(rt::TrackField::Year) == TrackColumnSizing::Fixed);
    CHECK(trackFieldColumnSizing(rt::TrackField::Bitrate) == TrackColumnSizing::Fixed);
    CHECK(trackFieldColumnSizing(rt::TrackField::TechnicalSummary) == TrackColumnSizing::Fixed);
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
      CHECK(trackFieldColumnAlignment(field) == TrackColumnAlignment::End);
    }

    for (auto const field : {rt::TrackField::Title,
                             rt::TrackField::Artist,
                             rt::TrackField::Tags,
                             rt::TrackField::FilePath,
                             rt::TrackField::Codec,
                             rt::TrackField::TechnicalSummary,
                             rt::TrackField::Quality})
    {
      CHECK(trackFieldColumnAlignment(field) == TrackColumnAlignment::Start);
    }
  }

  TEST_CASE("minimumTrackFieldColumnWidth keeps fixed minimums below default widths",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(minimumTrackFieldColumnWidth(rt::TrackField::Title) == 72);
    CHECK(minimumTrackFieldColumnWidth(rt::TrackField::Duration) == 40);
    CHECK(minimumTrackFieldColumnWidth(rt::TrackField::Duration) <
          defaultTrackFieldColumnWidth(rt::TrackField::Duration));
    CHECK(minimumTrackFieldColumnWidth(rt::TrackField::Year) < defaultTrackFieldColumnWidth(rt::TrackField::Year));
  }

  TEST_CASE("defaultTrackFieldColumnWeight favors title over secondary text fields",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(defaultTrackFieldColumnWeight(rt::TrackField::Title) > defaultTrackFieldColumnWeight(rt::TrackField::Artist));
    CHECK(defaultTrackFieldColumnWeight(rt::TrackField::Artist) ==
          defaultTrackFieldColumnWeight(rt::TrackField::Album));
    CHECK(defaultTrackFieldColumnWeight(rt::TrackField::Tags) > defaultTrackFieldColumnWeight(rt::TrackField::Genre));
  }

  TEST_CASE("trackFieldColumnTitle returns runtime field labels", "[uimodel][unit][library][presentation]")
  {
    CHECK(trackFieldColumnTitle(rt::TrackField::Title) == "Title");
    CHECK(trackFieldColumnTitle(rt::TrackField::Artist) == "Artist");
    CHECK(trackFieldColumnTitle(rt::TrackField::Duration) == "Duration");
  }

  TEST_CASE("presentable runtime fields have default presentation policy", "[uimodel][unit][library][presentation]")
  {
    for (auto const& rtDef : rt::trackFieldDefinitions())
    {
      if (!rtDef.presentable)
      {
        continue;
      }

      INFO("Field " << rtDef.id << " must have a column title and width");
      CHECK_FALSE(trackFieldColumnTitle(rtDef.field).empty());
      CHECK(defaultTrackFieldColumnWidth(rtDef.field) > 0);
    }
  }
} // namespace ao::uimodel::test
