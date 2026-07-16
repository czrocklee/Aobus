// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackPresentationRecommender - heuristics", "[uimodel][unit][library][presentation]")
  {
    auto const builtins = rt::builtinTrackPresentationPresets();
    auto customs = std::vector<rt::CustomTrackPresentationPreset>{};

    auto recommendSmart = [&](std::string const& filter)
    {
      auto const context = ListPresentationContext{
        .sourceKind = ListPresentationSourceKind::Smart,
        .smartListFilter = filter,
      };
      return recommendPresentation(context, builtins, customs).id;
    };

    SECTION("manual list defaults to List Order")
    {
      auto const context = ListPresentationContext{
        .listId = ListId{10},
        .sourceKind = ListPresentationSourceKind::Manual,
        .smartListFilter = "$composer = \"Bach\"",
      };
      auto const result = recommendPresentation(context, builtins, customs);

      CHECK(result.id == rt::kListOrderTrackPresentationId);
    }

    SECTION("All Tracks retains the normal albums fallback")
    {
      auto const context = ListPresentationContext{
        .listId = rt::kAllTracksListId,
        .sourceKind = ListPresentationSourceKind::AllTracks,
      };
      auto const result = recommendPresentation(context, builtins, customs);

      CHECK(result.id == "albums");
    }

    SECTION("empty filter falls back to albums")
    {
      CHECK(recommendSmart("") == "albums");
    }

    SECTION("classical composer")
    {
      CHECK(recommendSmart("$composer = \"Bach\"") == "classical-composers");
    }

    SECTION("classical work")
    {
      CHECK(recommendSmart("$work = \"Symphony 9\"") == "classical-works");
    }

    SECTION("technical fields")
    {
      CHECK(recommendSmart("@sampleRate >= 96000") == "technical");
      CHECK(recommendSmart("@bitDepth = 24") == "technical");
      CHECK(recommendSmart("@bitrate > 320000") == "technical");
    }

    SECTION("query aliases use the same recommendation signals")
    {
      CHECK(recommendSmart("$w = \"Symphony 9\"") == "classical-works");
      CHECK(recommendSmart("@sr >= 96000") == "technical");
    }

    SECTION("tag")
    {
      CHECK(recommendSmart("#tag = \"favorite\"") == "tagging");
    }

    SECTION("genre")
    {
      CHECK(recommendSmart("$genre = \"Rock\"") == "albums");
    }

    SECTION("year")
    {
      CHECK(recommendSmart("$year = 1990") == "albums");
    }

    SECTION("album artist")
    {
      CHECK(recommendSmart("$albumArtist = \"Artist\"") == "artists");
    }

    SECTION("artist")
    {
      CHECK(recommendSmart("$artist = \"Artist\"") == "albums");
    }

    SECTION("album")
    {
      CHECK(recommendSmart("$album = \"Album\"") == "albums");
    }

    SECTION("mixed fields defaults to highest priority")
    {
      // work > composer > technical > tag > genre...
      CHECK(recommendSmart("$work = \"A\" and $composer = \"B\"") == "classical-works");
      CHECK(recommendSmart("$genre = \"Rock\" and #tag = \"fave\"") == "tagging");
    }

    SECTION("invalid expression falls back")
    {
      CHECK(recommendSmart("invalid syntax") == "albums");
    }
  }
} // namespace ao::uimodel::test
