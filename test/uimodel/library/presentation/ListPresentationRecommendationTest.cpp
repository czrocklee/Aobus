// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentationRecommendation - heuristics")
  {
    auto const syntheticAlbums = rt::TrackPresentationSpec{
      .presetId = rt::BuiltinPreset::Albums,
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album, .ascending = true}},
      .visibleFields = {rt::TrackField::Album, rt::TrackField::Title},
      .redundantFields = {rt::TrackField::AlbumArtist},
    };
    auto const syntheticArtists = rt::TrackPresentationSpec{
      .presetId = rt::BuiltinPreset::Artists,
      .groupBy = rt::TrackGroupKey::AlbumArtist,
      .sortBy = {{.field = rt::TrackSortField::AlbumArtist, .ascending = true}},
      .visibleFields = {rt::TrackField::AlbumArtist, rt::TrackField::Album},
      .redundantFields = {rt::TrackField::Artist},
    };
    auto const syntheticClassicalComposers = rt::TrackPresentationSpec{
      .presetId = rt::BuiltinPreset::ClassicalComposers,
      .groupBy = rt::TrackGroupKey::Composer,
      .sortBy = {{.field = rt::TrackSortField::Composer, .ascending = false}},
      .visibleFields = {rt::TrackField::Composer, rt::TrackField::Work},
      .redundantFields = {rt::TrackField::Composer},
    };
    auto const syntheticClassicalWorks = rt::TrackPresentationSpec{
      .presetId = rt::BuiltinPreset::ClassicalWorks,
      .groupBy = rt::TrackGroupKey::Work,
      .sortBy = {{.field = rt::TrackSortField::Work, .ascending = false}},
      .visibleFields = {rt::TrackField::Work, rt::TrackField::Movement},
      .redundantFields = {rt::TrackField::Work},
    };
    auto const syntheticTagging = rt::TrackPresentationSpec{
      .presetId = rt::BuiltinPreset::Tagging,
      .groupBy = rt::TrackGroupKey::Genre,
      .sortBy = {{.field = rt::TrackSortField::Genre, .ascending = false}},
      .visibleFields = {rt::TrackField::Tags, rt::TrackField::Title},
      .redundantFields = {rt::TrackField::Tags},
    };
    auto const syntheticTechnical = rt::TrackPresentationSpec{
      .presetId = rt::BuiltinPreset::Technical,
      .groupBy = rt::TrackGroupKey::None,
      .sortBy = {{.field = rt::TrackSortField::Duration, .ascending = false}},
      .visibleFields = {rt::TrackField::TechnicalSummary, rt::TrackField::FilePath},
      .redundantFields = {rt::TrackField::FileSize},
    };
    auto const shipped = rt::builtinTrackPresentationPresets();
    auto const useSynthetic = GENERATE(false, true);
    INFO("Synthetic input catalog: " << useSynthetic);
    auto const builtins = useSynthetic
                            ? std::vector<rt::TrackPresentationPreset>{
                                {.spec = syntheticAlbums},
                                {.spec = syntheticArtists},
                                {.spec = syntheticClassicalComposers},
                                {.spec = syntheticClassicalWorks},
                                {.spec = syntheticTagging},
                                {.spec = syntheticTechnical},
                              }
                            : std::vector<rt::TrackPresentationPreset>{shipped.begin(), shipped.end()};
    auto const requireExpectedSpec = [&](rt::BuiltinPreset const id) -> rt::TrackPresentationSpec const&
    {
      auto const found = std::ranges::find(builtins, id, &rt::TrackPresentationPreset::presetId);
      REQUIRE(found != builtins.end());
      return found->spec;
    };
    auto const& expectedAlbums = requireExpectedSpec(rt::BuiltinPreset::Albums);
    auto const& expectedArtists = requireExpectedSpec(rt::BuiltinPreset::Artists);
    auto const& expectedClassicalComposers = requireExpectedSpec(rt::BuiltinPreset::ClassicalComposers);
    auto const& expectedClassicalWorks = requireExpectedSpec(rt::BuiltinPreset::ClassicalWorks);
    auto const& expectedTagging = requireExpectedSpec(rt::BuiltinPreset::Tagging);
    auto const& expectedTechnical = requireExpectedSpec(rt::BuiltinPreset::Technical);

    auto recommendSaved = [&](std::string const& filter)
    {
      auto const context = ListPresentationContext{
        .sourceKind = ListPresentationSourceKind::SavedList,
        .listExpression = filter,
      };
      return recommendListPresentation(context, builtins);
    };

    SECTION("saved list uses its expression")
    {
      auto const context = ListPresentationContext{
        .listId = ListId{10},
        .sourceKind = ListPresentationSourceKind::SavedList,
        .listExpression = "$composer = \"Bach\"",
      };
      auto const result = recommendListPresentation(context, builtins);

      CHECK(result == expectedClassicalComposers);
    }

    SECTION("All Tracks retains the normal albums fallback")
    {
      auto const context = ListPresentationContext{
        .listId = rt::kAllTracksListId,
        .sourceKind = ListPresentationSourceKind::AllTracks,
      };
      auto const result = recommendListPresentation(context, builtins);

      CHECK(result == expectedAlbums);
    }

    SECTION("empty filter falls back to albums")
    {
      CHECK(recommendSaved("") == expectedAlbums);
    }

    SECTION("classical composer")
    {
      CHECK(recommendSaved("$composer = \"Bach\"") == expectedClassicalComposers);
    }

    SECTION("classical work")
    {
      CHECK(recommendSaved("$work = \"Symphony 9\"") == expectedClassicalWorks);
    }

    SECTION("technical fields")
    {
      CHECK(recommendSaved("@sampleRate >= 96000") == expectedTechnical);
      CHECK(recommendSaved("@bitDepth = 24") == expectedTechnical);
      CHECK(recommendSaved("@bitrate > 320000") == expectedTechnical);
    }

    SECTION("query aliases use the same recommendation signals")
    {
      CHECK(recommendSaved("$w = \"Symphony 9\"") == expectedClassicalWorks);
      CHECK(recommendSaved("@sr >= 96000") == expectedTechnical);
    }

    SECTION("tag")
    {
      CHECK(recommendSaved("#tag = \"favorite\"") == expectedTagging);
    }

    SECTION("genre")
    {
      CHECK(recommendSaved("$genre = \"Rock\"") == expectedAlbums);
    }

    SECTION("year")
    {
      CHECK(recommendSaved("$year = 1990") == expectedAlbums);
    }

    SECTION("album artist")
    {
      CHECK(recommendSaved("$albumArtist = \"Artist\"") == expectedArtists);
    }

    SECTION("artist")
    {
      CHECK(recommendSaved("$artist = \"Artist\"") == expectedAlbums);
    }

    SECTION("album")
    {
      CHECK(recommendSaved("$album = \"Album\"") == expectedAlbums);
    }

    SECTION("mixed fields defaults to highest priority")
    {
      // work > composer > technical > tag > genre...
      CHECK(recommendSaved("$work = \"A\" and $composer = \"B\"") == expectedClassicalWorks);
      CHECK(recommendSaved("$composer = \"B\" and @sampleRate >= 96000") == expectedClassicalComposers);
      CHECK(recommendSaved("@bitDepth = 24 and #tag = \"fave\"") == expectedTechnical);
      CHECK(recommendSaved("$genre = \"Rock\" and #tag = \"fave\"") == expectedTagging);
      CHECK(recommendSaved("$genre = \"Rock\" and $albumArtist = \"Artist\"") == expectedAlbums);
      CHECK(recommendSaved("$year = 1990 and $albumArtist = \"Artist\"") == expectedAlbums);
    }

    SECTION("invalid expression falls back")
    {
      CHECK(recommendSaved("invalid syntax") == expectedAlbums);
    }

    SECTION("missing recommendation id falls back to the first input spec")
    {
      auto const albumsOnly = std::vector<rt::TrackPresentationPreset>{{.spec = expectedAlbums}};
      auto const context = ListPresentationContext{
        .sourceKind = ListPresentationSourceKind::SavedList,
        .listExpression = "$composer = \"Bach\"",
      };

      CHECK(recommendListPresentation(context, albumsOnly) == expectedAlbums);
    }
  }
} // namespace ao::uimodel::test
