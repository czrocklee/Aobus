// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentationRecommendation - heuristics", "[uimodel][unit][library][presentation]")
  {
    auto const builtins = rt::builtinTrackPresentationPresets();
    auto customs = std::vector<rt::CustomTrackPresentationPreset>{};

    auto recommendSaved = [&](std::string const& filter)
    {
      auto const context = ListPresentationContext{
        .sourceKind = ListPresentationSourceKind::SavedList,
        .listExpression = filter,
      };
      return recommendListPresentation(context, builtins, customs).id;
    };

    SECTION("saved list uses its expression")
    {
      auto const context = ListPresentationContext{
        .listId = ListId{10},
        .sourceKind = ListPresentationSourceKind::SavedList,
        .listExpression = "$composer = \"Bach\"",
      };
      auto const result = recommendListPresentation(context, builtins, customs);

      CHECK(result.id == "classical-composers");
    }

    SECTION("All Tracks retains the normal albums fallback")
    {
      auto const context = ListPresentationContext{
        .listId = rt::kAllTracksListId,
        .sourceKind = ListPresentationSourceKind::AllTracks,
      };
      auto const result = recommendListPresentation(context, builtins, customs);

      CHECK(result.id == "albums");
    }

    SECTION("empty filter falls back to albums")
    {
      CHECK(recommendSaved("") == "albums");
    }

    SECTION("classical composer")
    {
      CHECK(recommendSaved("$composer = \"Bach\"") == "classical-composers");
    }

    SECTION("classical work")
    {
      CHECK(recommendSaved("$work = \"Symphony 9\"") == "classical-works");
    }

    SECTION("technical fields")
    {
      CHECK(recommendSaved("@sampleRate >= 96000") == "technical");
      CHECK(recommendSaved("@bitDepth = 24") == "technical");
      CHECK(recommendSaved("@bitrate > 320000") == "technical");
    }

    SECTION("query aliases use the same recommendation signals")
    {
      CHECK(recommendSaved("$w = \"Symphony 9\"") == "classical-works");
      CHECK(recommendSaved("@sr >= 96000") == "technical");
    }

    SECTION("tag")
    {
      CHECK(recommendSaved("#tag = \"favorite\"") == "tagging");
    }

    SECTION("genre")
    {
      CHECK(recommendSaved("$genre = \"Rock\"") == "albums");
    }

    SECTION("year")
    {
      CHECK(recommendSaved("$year = 1990") == "albums");
    }

    SECTION("album artist")
    {
      CHECK(recommendSaved("$albumArtist = \"Artist\"") == "artists");
    }

    SECTION("artist")
    {
      CHECK(recommendSaved("$artist = \"Artist\"") == "albums");
    }

    SECTION("album")
    {
      CHECK(recommendSaved("$album = \"Album\"") == "albums");
    }

    SECTION("mixed fields defaults to highest priority")
    {
      // work > composer > technical > tag > genre...
      CHECK(recommendSaved("$work = \"A\" and $composer = \"B\"") == "classical-works");
      CHECK(recommendSaved("$genre = \"Rock\" and #tag = \"fave\"") == "tagging");
    }

    SECTION("invalid expression falls back")
    {
      CHECK(recommendSaved("invalid syntax") == "albums");
    }
  }
} // namespace ao::uimodel::test
