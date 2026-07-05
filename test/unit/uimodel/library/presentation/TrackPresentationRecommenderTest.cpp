// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackPresentation.h>
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

    auto recommend = [&](std::string const& filter) { return recommendPresentation(filter, builtins, customs).id; };

    SECTION("empty filter falls back to albums")
    {
      CHECK(recommend("") == "albums");
    }

    SECTION("classical composer")
    {
      CHECK(recommend("$composer = \"Bach\"") == "classical-composers");
    }

    SECTION("classical work")
    {
      CHECK(recommend("$work = \"Symphony 9\"") == "classical-works");
    }

    SECTION("technical fields")
    {
      CHECK(recommend("@sampleRate >= 96000") == "technical");
      CHECK(recommend("@bitDepth = 24") == "technical");
      CHECK(recommend("@bitrate > 320000") == "technical");
    }

    SECTION("tag")
    {
      CHECK(recommend("#tag = \"favorite\"") == "tagging");
    }

    SECTION("genre")
    {
      CHECK(recommend("$genre = \"Rock\"") == "albums");
    }

    SECTION("year")
    {
      CHECK(recommend("$year = 1990") == "albums");
    }

    SECTION("album artist")
    {
      CHECK(recommend("$albumArtist = \"Artist\"") == "artists");
    }

    SECTION("artist")
    {
      CHECK(recommend("$artist = \"Artist\"") == "albums");
    }

    SECTION("album")
    {
      CHECK(recommend("$album = \"Album\"") == "albums");
    }

    SECTION("mixed fields defaults to highest priority")
    {
      // work > composer > technical > tag > genre...
      CHECK(recommend("$work = \"A\" and $composer = \"B\"") == "classical-works");
      CHECK(recommend("$genre = \"Rock\" and #tag = \"fave\"") == "tagging");
    }

    SECTION("invalid expression falls back")
    {
      CHECK(recommend("invalid syntax") == "albums");
    }
  }
} // namespace ao::uimodel::test
