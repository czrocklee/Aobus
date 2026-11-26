// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/TrackPresentation.h>

#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    // Field-wise checks name the part of the spec that differs; the spec itself has no Catch2 printer.
    void checkSpec(TrackPresentationSpec const& actual, TrackPresentationSpec const& expected)
    {
      CHECK(actual.presetId == expected.presetId);
      CHECK(actual.groupBy == expected.groupBy);
      CHECK(actual.sortBy == expected.sortBy);
      CHECK(actual.optColumnSort == expected.optColumnSort);
      CHECK(actual.visibleFields == expected.visibleFields);
      CHECK(actual.redundantFields == expected.redundantFields);
    }
  } // namespace

  TEST_CASE("builtinTrackPresentationPresets preserves ordered catalog contracts")
  {
    auto const presets = builtinTrackPresentationPresets();
    constexpr auto kExpectedIds = std::to_array<std::string_view>({
      "library",
      "list-order",
      "songs",
      "albums",
      "artists",
      "performers",
      "genres",
      "years",
      "classical-composers",
      "classical-conductors",
      "classical-works",
      "tagging",
      "technical",
    });

    REQUIRE(presets.size() == kExpectedIds.size());

    for (std::size_t index = 0; index < presets.size(); ++index)
    {
      auto const id = presets[index].presetId();
      CHECK(builtinPresetKey(id) == kExpectedIds[index]);
      CHECK(parseTrackPresetId(kExpectedIds[index]) == TrackPresetId{id});
      CHECK(&builtinTrackPresentationPreset(id) == &presets[index]);

      for (auto const& term : presets[index].spec.sortBy)
      {
        CHECK(term.ascending);
      }
    }

    // "album-artists" was renamed to "artists"; the former "artists" is now "performers".
    CHECK_FALSE(parseTrackPresetId("album-artists"));
  }

  TEST_CASE("Custom preset keys - preserve all 64 bits and reject noncanonical text")
  {
    auto const custom = CustomPresetId{0xfedcba9876543210ULL};
    CHECK(trackPresetKey(custom) == "fedcba9876543210");
    CHECK(parseTrackPresetId("fedcba9876543210") == TrackPresetId{custom});
    CHECK(trackPresetKey(CustomPresetId{0}) == "0000000000000000");
    CHECK(parseTrackPresetId("0000000000000000") == TrackPresetId{CustomPresetId{0}});
    CHECK_FALSE(parseTrackPresetId("FEDCBA9876543210"));
    CHECK_FALSE(parseTrackPresetId("0"));
    CHECK_FALSE(parseTrackPresetId("0000000000000000a"));
    CHECK_FALSE(parseTrackPresetId("000000000000000z"));
    CHECK_FALSE(parseTrackPresetId("ablums"));
  }

  TEST_CASE("defaultTrackPresentationSpec is the library preset")
  {
    checkSpec(defaultTrackPresentationSpec(), builtinTrackPresentationPreset(BuiltinPreset::Library).spec);
  }

  TEST_CASE("library preset - orders by album artist and stays album-intact")
  {
    // First key is AlbumArtist so various-artist compilations are not split apart.
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Library).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Library,
                .groupBy = TrackGroupKey::None,
                .sortBy = {{TrackSortField::AlbumArtist, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::DisplayTrackNumber,
                                  TrackField::Title,
                                  TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::Year,
                                  TrackField::Duration},
                .redundantFields = {},
              });
  }

  TEST_CASE("list-order preset - preserves source order with track columns")
  {
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::ListOrder).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::ListOrder,
                .groupBy = TrackGroupKey::None,
                .sortBy = {},
                .visibleFields = {TrackField::DisplayTrackNumber,
                                  TrackField::Title,
                                  TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::Year,
                                  TrackField::Duration},
                .redundantFields = {},
              });
  }

  TEST_CASE("songs preset - is a flat title-ordered list")
  {
    checkSpec(
      builtinTrackPresentationPreset(BuiltinPreset::Songs).spec,
      TrackPresentationSpec{
        .presetId = BuiltinPreset::Songs,
        .groupBy = TrackGroupKey::None,
        .sortBy = {{TrackSortField::Title, true}, {TrackSortField::Artist, true}, {TrackSortField::Album, true}},
        .visibleFields =
          {TrackField::Title, TrackField::Artist, TrackField::Album, TrackField::Duration, TrackField::Year},
        .redundantFields = {},
      });
  }

  TEST_CASE("albums preset - groups tracks by album")
  {
    checkSpec(
      builtinTrackPresentationPreset(BuiltinPreset::Albums).spec,
      TrackPresentationSpec{
        .presetId = BuiltinPreset::Albums,
        .groupBy = TrackGroupKey::Album,
        .sortBy = {{TrackSortField::AlbumArtist, true},
                   {TrackSortField::Album, true},
                   {TrackSortField::DiscNumber, true},
                   {TrackSortField::TrackNumber, true},
                   {TrackSortField::Title, true}},
        .visibleFields = {TrackField::DisplayTrackNumber, TrackField::Title, TrackField::Artist, TrackField::Duration},
        .redundantFields = {TrackField::Album, TrackField::AlbumArtist},
      });
  }

  TEST_CASE("artists preset - groups by album artist")
  {
    // Year leads the columns so it reads as a discography.
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Artists).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Artists,
                .groupBy = TrackGroupKey::AlbumArtist,
                .sortBy = {{TrackSortField::AlbumArtist, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Year,
                                  TrackField::Album,
                                  TrackField::DisplayTrackNumber,
                                  TrackField::Title,
                                  TrackField::Artist,
                                  TrackField::Duration},
                .redundantFields = {TrackField::AlbumArtist},
              });
  }

  TEST_CASE("performers preset - groups by track artist")
  {
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Performers).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Performers,
                .groupBy = TrackGroupKey::Artist,
                .sortBy = {{TrackSortField::Artist, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Year,
                                  TrackField::Album,
                                  TrackField::DisplayTrackNumber,
                                  TrackField::Title,
                                  TrackField::Duration},
                .redundantFields = {TrackField::Artist},
              });
  }

  TEST_CASE("genres preset - groups albums within each genre")
  {
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Genres).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Genres,
                .groupBy = TrackGroupKey::Genre,
                .sortBy = {{TrackSortField::Genre, true},
                           {TrackSortField::AlbumArtist, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::DisplayTrackNumber,
                                  TrackField::Title,
                                  TrackField::Year,
                                  TrackField::Duration},
                .redundantFields = {TrackField::Genre},
              });
  }

  TEST_CASE("years preset - groups albums by release year")
  {
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Years).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Years,
                .groupBy = TrackGroupKey::Year,
                .sortBy = {{TrackSortField::Year, true},
                           {TrackSortField::AlbumArtist, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::DisplayTrackNumber,
                                  TrackField::Title,
                                  TrackField::Genre,
                                  TrackField::Duration},
                .redundantFields = {TrackField::Year},
              });
  }

  TEST_CASE("classical-composers preset - groups works by composer")
  {
    // Work leads the columns; DisplayTrackNumber is demoted to the end because
    // in a classical context the movement is more meaningful than the track number.
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::ClassicalComposers).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::ClassicalComposers,
                .groupBy = TrackGroupKey::Composer,
                .sortBy = {{TrackSortField::Composer, true},
                           {TrackSortField::Work, true},
                           {TrackSortField::RecordingYear, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::Movement, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Work,
                                  TrackField::Movement,
                                  TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::Year,
                                  TrackField::Duration,
                                  TrackField::DisplayTrackNumber,
                                  TrackField::RecordingDate},
                .redundantFields = {TrackField::Composer},
              });
  }

  TEST_CASE("classical-conductors preset - groups works by conductor")
  {
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::ClassicalConductors).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::ClassicalConductors,
                .groupBy = TrackGroupKey::Conductor,
                .sortBy = {{TrackSortField::Conductor, true},
                           {TrackSortField::Composer, true},
                           {TrackSortField::Work, true},
                           {TrackSortField::RecordingYear, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::Movement, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Work,
                                  TrackField::Movement,
                                  TrackField::Composer,
                                  TrackField::Conductor,
                                  TrackField::Ensemble,
                                  TrackField::Album,
                                  TrackField::Year,
                                  TrackField::Duration,
                                  TrackField::RecordingDate},
                .redundantFields = {},
              });
  }

  TEST_CASE("classical-works preset - groups movements by work")
  {
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::ClassicalWorks).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::ClassicalWorks,
                .groupBy = TrackGroupKey::Work,
                .sortBy = {{TrackSortField::Composer, true},
                           {TrackSortField::Work, true},
                           {TrackSortField::RecordingYear, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::Movement, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::DisplayTrackNumber,
                                  TrackField::Movement,
                                  TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::Year,
                                  TrackField::Duration,
                                  TrackField::RecordingDate},
                .redundantFields = {TrackField::Composer, TrackField::Work},
              });
  }

  TEST_CASE("tagging preset - has all curation columns visible")
  {
    // Curation view exposes raw disc/track numbers rather than the formatted
    // DisplayTrackNumber, so tagging mistakes (missing disc, wrong totals) are visible.
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Tagging).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Tagging,
                .groupBy = TrackGroupKey::None,
                .sortBy = {{TrackSortField::Artist, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::DiscNumber,
                                  TrackField::TrackNumber,
                                  TrackField::Title,
                                  TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::Genre,
                                  TrackField::Year,
                                  TrackField::Duration,
                                  TrackField::Tags},
                .redundantFields = {},
              });
  }

  TEST_CASE("technical preset - exposes file inspection columns")
  {
    // FileSize/ModifiedTime are manifest-sourced and not yet sortable, so the
    // preset falls back to a metadata-only sort order.
    checkSpec(builtinTrackPresentationPreset(BuiltinPreset::Technical).spec,
              TrackPresentationSpec{
                .presetId = BuiltinPreset::Technical,
                .groupBy = TrackGroupKey::None,
                .sortBy = {{TrackSortField::AlbumArtist, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Title,
                                  TrackField::Artist,
                                  TrackField::Album,
                                  TrackField::TechnicalSummary,
                                  TrackField::FileSize,
                                  TrackField::FilePath},
                .redundantFields = {},
              });
  }

  TEST_CASE("normalizeTrackPresentationSpec removes duplicate visible fields")
  {
    auto spec = TrackPresentationSpec{
      .presetId = BuiltinPreset::Songs,
      .sortBy = {{TrackSortField::Title, true}},
      .optColumnSort = TrackColumnSort{TrackField::Title, false},
      .visibleFields =
        {
          TrackField::Title,
          TrackField::Artist,
          TrackField::Title,
          TrackField::Album,
          TrackField::Artist,
        },
    };

    auto normalized = normalizeTrackPresentationSpec(spec);

    CHECK(normalized.isPreset(BuiltinPreset::Songs));
    CHECK(normalized.optColumnSort == spec.optColumnSort);
    REQUIRE(normalized.visibleFields.size() == 3);
    CHECK(normalized.visibleFields[0] == TrackField::Title);
    CHECK(normalized.visibleFields[1] == TrackField::Artist);
    CHECK(normalized.visibleFields[2] == TrackField::Album);
  }

  TEST_CASE("normalizeTrackPresentationSpec keeps at least one visible field")
  {
    auto spec = TrackPresentationSpec{.presetId = CustomPresetId{1}};

    auto normalized = normalizeTrackPresentationSpec(spec);

    CHECK(normalized.isPreset(CustomPresetId{1}));
    REQUIRE(normalized.visibleFields.size() == 1);
    CHECK(normalized.visibleFields[0] == TrackField::Title);
  }

  TEST_CASE("normalizeTrackPresentationSpec removes duplicate redundant fields")
  {
    auto spec = TrackPresentationSpec{
      .presetId = CustomPresetId{2},
      .redundantFields =
        {
          TrackField::Album,
          TrackField::AlbumArtist,
          TrackField::Album,
        },
    };

    auto normalized = normalizeTrackPresentationSpec(spec);

    REQUIRE(normalized.redundantFields.size() == 2);
    CHECK(normalized.redundantFields[0] == TrackField::Album);
    CHECK(normalized.redundantFields[1] == TrackField::AlbumArtist);
  }

  TEST_CASE("normalizeTrackPresentationSpec keeps category columns despite primary-name grouping")
  {
    auto const normalized = normalizeTrackPresentationSpec(TrackPresentationSpec{
      .groupBy = TrackGroupKey::Conductor,
      .visibleFields = {TrackField::Title, TrackField::Conductor, TrackField::Ensemble, TrackField::Soloist},
      .redundantFields = {TrackField::Conductor, TrackField::Ensemble, TrackField::Soloist, TrackField::Album},
    });
    CHECK(normalized.visibleFields ==
          std::vector<TrackField>{TrackField::Title, TrackField::Conductor, TrackField::Ensemble, TrackField::Soloist});
    CHECK(normalized.redundantFields == std::vector<TrackField>{TrackField::Album});
  }

  TEST_CASE("TrackPresentationSpec - both track number columns move disc and track ahead of preset ties")
  {
    auto spec = builtinTrackPresentationPreset(BuiltinPreset::Library).spec;

    SECTION("track number")
    {
      spec.optColumnSort = TrackColumnSort{TrackField::TrackNumber, false};
    }

    SECTION("display track number")
    {
      spec.optColumnSort = TrackColumnSort{TrackField::DisplayTrackNumber, false};
    }

    auto const expected = std::vector<TrackSortTerm>{
      {TrackSortField::DiscNumber, false},
      {TrackSortField::TrackNumber, false},
      {TrackSortField::AlbumArtist, true},
      {TrackSortField::Album, true},
      {TrackSortField::Title, true},
    };
    CHECK(spec.effectiveSortBy() == expected);
  }

  TEST_CASE("TrackPresentationSpec - starts with the library identity")
  {
    CHECK(TrackPresentationSpec{}.isPreset(BuiltinPreset::Library));
  }
} // namespace ao::rt::test
