// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/TrackPresentation.h>

#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  namespace
  {
    TrackPresentationSpec const& requirePresetSpec(std::string_view const id)
    {
      auto const* preset = builtinTrackPresentationPreset(id);
      REQUIRE(preset != nullptr);
      return preset->spec;
    }

    // Field-wise checks name the part of the spec that differs; the spec itself has no Catch2 printer.
    void checkSpec(TrackPresentationSpec const& actual, TrackPresentationSpec const& expected)
    {
      CHECK(actual.id == expected.id);
      CHECK(actual.groupBy == expected.groupBy);
      CHECK(actual.sortBy == expected.sortBy);
      CHECK(actual.visibleFields == expected.visibleFields);
      CHECK(actual.redundantFields == expected.redundantFields);
    }
  } // namespace

  TEST_CASE("builtinTrackPresentationPresets preserves ordered catalog contracts", "[runtime][unit][presentation]")
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
      CHECK(presets[index].spec.id == kExpectedIds[index]);

      for (auto const& term : presets[index].spec.sortBy)
      {
        CHECK(term.ascending);
      }
    }

    // "album-artists" was renamed to "artists"; the former "artists" is now "performers".
    CHECK(builtinTrackPresentationPreset("album-artists") == nullptr);
  }

  TEST_CASE("builtinTrackPresentationPreset lookup by id", "[runtime][unit][presentation]")
  {
    CHECK(builtinTrackPresentationPreset("songs") != nullptr);
    CHECK(builtinTrackPresentationPreset("albums") != nullptr);
    CHECK(builtinTrackPresentationPreset(kListOrderTrackPresentationId) != nullptr);
    CHECK(builtinTrackPresentationPreset("nonexistent") == nullptr);
  }

  TEST_CASE("defaultTrackPresentationSpec is the library preset", "[runtime][unit][presentation]")
  {
    checkSpec(defaultTrackPresentationSpec(), requirePresetSpec("library"));
  }

  TEST_CASE("library preset - orders by album artist and stays album-intact", "[runtime][unit][presentation]")
  {
    // First key is AlbumArtist so various-artist compilations are not split apart.
    checkSpec(requirePresetSpec("library"),
              TrackPresentationSpec{
                .id = "library",
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

  TEST_CASE("list-order preset - preserves source order with track columns", "[runtime][unit][presentation]")
  {
    checkSpec(requirePresetSpec(kListOrderTrackPresentationId),
              TrackPresentationSpec{
                .id = std::string{kListOrderTrackPresentationId},
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

  TEST_CASE("songs preset - is a flat title-ordered list", "[runtime][unit][presentation]")
  {
    checkSpec(
      requirePresetSpec("songs"),
      TrackPresentationSpec{
        .id = "songs",
        .groupBy = TrackGroupKey::None,
        .sortBy = {{TrackSortField::Title, true}, {TrackSortField::Artist, true}, {TrackSortField::Album, true}},
        .visibleFields =
          {TrackField::Title, TrackField::Artist, TrackField::Album, TrackField::Duration, TrackField::Year},
        .redundantFields = {},
      });
  }

  TEST_CASE("albums preset - groups tracks by album", "[runtime][unit][presentation]")
  {
    checkSpec(
      requirePresetSpec("albums"),
      TrackPresentationSpec{
        .id = "albums",
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

  TEST_CASE("artists preset - groups by album artist", "[runtime][unit][presentation]")
  {
    // Year leads the columns so it reads as a discography.
    checkSpec(requirePresetSpec("artists"),
              TrackPresentationSpec{
                .id = "artists",
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

  TEST_CASE("performers preset - groups by track artist", "[runtime][unit][presentation]")
  {
    checkSpec(requirePresetSpec("performers"),
              TrackPresentationSpec{
                .id = "performers",
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

  TEST_CASE("genres preset - groups albums within each genre", "[runtime][unit][presentation]")
  {
    checkSpec(requirePresetSpec("genres"),
              TrackPresentationSpec{
                .id = "genres",
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

  TEST_CASE("years preset - groups albums by release year", "[runtime][unit][presentation]")
  {
    checkSpec(requirePresetSpec("years"),
              TrackPresentationSpec{
                .id = "years",
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

  TEST_CASE("classical-composers preset - groups works by composer", "[runtime][unit][presentation]")
  {
    // Work leads the columns; DisplayTrackNumber is demoted to the end because
    // in a classical context the movement is more meaningful than the track number.
    checkSpec(requirePresetSpec("classical-composers"),
              TrackPresentationSpec{
                .id = "classical-composers",
                .groupBy = TrackGroupKey::Composer,
                .sortBy = {{TrackSortField::Composer, true},
                           {TrackSortField::Work, true},
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
                                  TrackField::DisplayTrackNumber},
                .redundantFields = {TrackField::Composer},
              });
  }

  TEST_CASE("classical-conductors preset - groups works by conductor", "[runtime][unit][presentation]")
  {
    checkSpec(requirePresetSpec("classical-conductors"),
              TrackPresentationSpec{
                .id = "classical-conductors",
                .groupBy = TrackGroupKey::Conductor,
                .sortBy = {{TrackSortField::Conductor, true},
                           {TrackSortField::Composer, true},
                           {TrackSortField::Work, true},
                           {TrackSortField::Year, true},
                           {TrackSortField::Album, true},
                           {TrackSortField::Movement, true},
                           {TrackSortField::DiscNumber, true},
                           {TrackSortField::TrackNumber, true},
                           {TrackSortField::Title, true}},
                .visibleFields = {TrackField::Work,
                                  TrackField::Movement,
                                  TrackField::Composer,
                                  TrackField::Ensemble,
                                  TrackField::Album,
                                  TrackField::Year,
                                  TrackField::Duration},
                .redundantFields = {TrackField::Conductor},
              });
  }

  TEST_CASE("classical-works preset - groups movements by work", "[runtime][unit][presentation]")
  {
    checkSpec(requirePresetSpec("classical-works"),
              TrackPresentationSpec{
                .id = "classical-works",
                .groupBy = TrackGroupKey::Work,
                .sortBy = {{TrackSortField::Composer, true},
                           {TrackSortField::Work, true},
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
                                  TrackField::Duration},
                .redundantFields = {TrackField::Composer, TrackField::Work},
              });
  }

  TEST_CASE("tagging preset - has all curation columns visible", "[runtime][unit][presentation]")
  {
    // Curation view exposes raw disc/track numbers rather than the formatted
    // DisplayTrackNumber, so tagging mistakes (missing disc, wrong totals) are visible.
    checkSpec(requirePresetSpec("tagging"),
              TrackPresentationSpec{
                .id = "tagging",
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

  TEST_CASE("technical preset - exposes file inspection columns", "[runtime][unit][presentation]")
  {
    // FileSize/ModifiedTime are manifest-sourced and not yet sortable, so the
    // preset falls back to a metadata-only sort order.
    checkSpec(requirePresetSpec("technical"),
              TrackPresentationSpec{
                .id = "technical",
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

  TEST_CASE("normalizeTrackPresentationSpec removes duplicate visible fields", "[runtime][unit][presentation]")
  {
    auto spec = TrackPresentationSpec{
      .id = "test",
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

    REQUIRE(normalized.visibleFields.size() == 3);
    CHECK(normalized.visibleFields[0] == TrackField::Title);
    CHECK(normalized.visibleFields[1] == TrackField::Artist);
    CHECK(normalized.visibleFields[2] == TrackField::Album);
  }

  TEST_CASE("normalizeTrackPresentationSpec keeps at least one visible field", "[runtime][unit][presentation]")
  {
    auto spec = TrackPresentationSpec{.id = "custom-empty"};

    auto normalized = normalizeTrackPresentationSpec(spec);

    CHECK(normalized.id == "custom-empty");
    REQUIRE(normalized.visibleFields.size() == 1);
    CHECK(normalized.visibleFields[0] == TrackField::Title);
  }

  TEST_CASE("normalizeTrackPresentationSpec removes duplicate redundant fields", "[runtime][unit][presentation]")
  {
    auto spec = TrackPresentationSpec{
      .id = "test",
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

  TEST_CASE("normalizeTrackPresentationSpec defaults empty id to library", "[runtime][unit][presentation]")
  {
    auto spec = TrackPresentationSpec{.id = ""};

    auto normalized = normalizeTrackPresentationSpec(spec);

    CHECK(normalized.id == "library");
  }
} // namespace ao::rt::test
