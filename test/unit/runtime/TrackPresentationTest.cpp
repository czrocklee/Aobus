// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace ao::rt::test
{
  TEST_CASE("builtinTrackPresentationPresets contains all expected ids", "[runtime][unit][presentation]")
  {
    auto presets = builtinTrackPresentationPresets();

    auto findPreset = [&](std::string_view id) -> bool
    { return std::ranges::contains(presets, id, [](TrackPresentationPreset const& p) { return p.spec.id; }); };

    CHECK(findPreset("library"));
    CHECK(findPreset("songs"));
    CHECK(findPreset("albums"));
    CHECK(findPreset("artists"));
    CHECK(findPreset("performers"));
    CHECK(findPreset("classical-composers"));
    CHECK(findPreset("classical-works"));
    CHECK(findPreset("genres"));
    CHECK(findPreset("years"));
    CHECK(findPreset("tagging"));
    CHECK(findPreset("technical"));

    // "album-artists" was renamed to "artists"; the former "artists" is now "performers".
    CHECK_FALSE(findPreset("album-artists"));
  }

  TEST_CASE("builtinTrackPresentationPreset lookup by id", "[runtime][unit][presentation]")
  {
    CHECK(builtinTrackPresentationPreset("songs") != nullptr);
    CHECK(builtinTrackPresentationPreset("albums") != nullptr);
    CHECK(builtinTrackPresentationPreset("nonexistent") == nullptr);
  }

  TEST_CASE("defaultTrackPresentationSpec is library", "[runtime][unit][presentation]")
  {
    auto spec = defaultTrackPresentationSpec();

    CHECK(spec.id == "library");
    CHECK(spec.groupBy == TrackGroupKey::None);
    CHECK(spec.redundantFields.empty());
  }

  TEST_CASE("library preset orders by album artist and stays album-intact", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("library");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::None);

    REQUIRE(spec.sortBy.size() == 5);
    // First key is AlbumArtist so various-artist compilations are not split apart.
    CHECK(spec.sortBy[0].field == TrackSortField::AlbumArtist);
    CHECK(spec.sortBy[1].field == TrackSortField::Album);
    CHECK(spec.sortBy[2].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[3].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[4].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 6);
    CHECK(spec.visibleFields[0] == TrackField::DisplayTrackNumber);
    CHECK(spec.visibleFields[1] == TrackField::Title);
    CHECK(spec.visibleFields[2] == TrackField::Artist);
    CHECK(spec.visibleFields[3] == TrackField::Album);
    CHECK(spec.visibleFields[4] == TrackField::Year);
    CHECK(spec.visibleFields[5] == TrackField::Duration);
    CHECK_FALSE(std::ranges::contains(spec.visibleFields, TrackField::Tags));

    CHECK(spec.redundantFields.empty());
  }

  TEST_CASE("songs preset is a flat title-ordered list", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("songs");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::None);

    REQUIRE(spec.sortBy.size() == 3);
    CHECK(spec.sortBy[0].field == TrackSortField::Title);
    CHECK(spec.sortBy[1].field == TrackSortField::Artist);
    CHECK(spec.sortBy[2].field == TrackSortField::Album);

    REQUIRE(spec.visibleFields.size() == 5);
    CHECK(spec.visibleFields[0] == TrackField::Title);
    CHECK(spec.visibleFields[1] == TrackField::Artist);
    CHECK(spec.visibleFields[2] == TrackField::Album);
    CHECK(spec.visibleFields[3] == TrackField::Duration);
    CHECK(spec.visibleFields[4] == TrackField::Year);

    CHECK(spec.redundantFields.empty());
  }

  TEST_CASE("artists preset groups by album artist", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("artists");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::AlbumArtist);
    REQUIRE(spec.sortBy.size() == 6);
    CHECK(spec.sortBy[0].field == TrackSortField::AlbumArtist);
    CHECK(spec.sortBy[1].field == TrackSortField::Year);

    // Year leads the columns so it reads as a discography.
    REQUIRE(spec.visibleFields.size() == 6);
    CHECK(spec.visibleFields[0] == TrackField::Year);
    CHECK(spec.visibleFields[1] == TrackField::Album);
    CHECK_FALSE(std::ranges::contains(spec.visibleFields, TrackField::Tags));

    REQUIRE(spec.redundantFields.size() == 1);
    CHECK(spec.redundantFields[0] == TrackField::AlbumArtist);
  }

  TEST_CASE("performers preset groups by track artist", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("performers");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::Artist);
    REQUIRE(spec.redundantFields.size() == 1);
    CHECK(spec.redundantFields[0] == TrackField::Artist);
    CHECK_FALSE(std::ranges::contains(spec.visibleFields, TrackField::Tags));
  }

  TEST_CASE("technical preset exposes file inspection columns", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("technical");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::None);

    REQUIRE(spec.visibleFields.size() == 6);
    CHECK(spec.visibleFields[0] == TrackField::Title);
    CHECK(spec.visibleFields[1] == TrackField::Artist);
    CHECK(spec.visibleFields[2] == TrackField::Album);
    CHECK(spec.visibleFields[3] == TrackField::TechnicalSummary);
    CHECK(spec.visibleFields[4] == TrackField::FileSize);
    CHECK(spec.visibleFields[5] == TrackField::FilePath);

    // FileSize/ModifiedTime are manifest-sourced and not yet sortable, so the
    // preset falls back to a metadata-only sort order.
    REQUIRE(spec.sortBy.size() == 5);
    CHECK(spec.sortBy[0].field == TrackSortField::AlbumArtist);
    CHECK(spec.sortBy[1].field == TrackSortField::Album);
  }

  TEST_CASE("albums preset has correct group, sort, and redundant fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::Album);

    REQUIRE(spec.sortBy.size() == 5);
    CHECK(spec.sortBy[0].field == TrackSortField::AlbumArtist);
    CHECK(spec.sortBy[1].field == TrackSortField::Album);
    CHECK(spec.sortBy[2].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[3].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[4].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 4);
    CHECK(spec.visibleFields[0] == TrackField::DisplayTrackNumber);
    CHECK(spec.visibleFields[1] == TrackField::Title);
    CHECK(spec.visibleFields[2] == TrackField::Artist);
    CHECK(spec.visibleFields[3] == TrackField::Duration);
    CHECK_FALSE(std::ranges::contains(spec.visibleFields, TrackField::Tags));

    REQUIRE(spec.redundantFields.size() == 2);
    CHECK(spec.redundantFields[0] == TrackField::Album);
    CHECK(spec.redundantFields[1] == TrackField::AlbumArtist);
  }

  TEST_CASE("classical-composers preset has correct sort and visible fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("classical-composers");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::Composer);

    REQUIRE(spec.sortBy.size() == 8);
    CHECK(spec.sortBy[0].field == TrackSortField::Composer);
    CHECK(spec.sortBy[1].field == TrackSortField::Work);
    CHECK(spec.sortBy[2].field == TrackSortField::Year);
    CHECK(spec.sortBy[3].field == TrackSortField::Album);
    CHECK(spec.sortBy[4].field == TrackSortField::Movement);
    CHECK(spec.sortBy[5].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[6].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[7].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 7);
    // Work leads the columns; DisplayTrackNumber is demoted to the end because
    // in a classical context the movement is more meaningful than the track number.
    CHECK(spec.visibleFields[0] == TrackField::Work);
    CHECK(spec.visibleFields[1] == TrackField::Movement);
    CHECK(spec.visibleFields[2] == TrackField::Artist);
    CHECK(spec.visibleFields[3] == TrackField::Album);
    CHECK(spec.visibleFields[4] == TrackField::Year);
    CHECK(spec.visibleFields[5] == TrackField::Duration);
    CHECK(spec.visibleFields[6] == TrackField::DisplayTrackNumber);

    REQUIRE(spec.redundantFields.size() == 1);
    CHECK(spec.redundantFields[0] == TrackField::Composer);
  }

  TEST_CASE("classical-works preset has correct group, sort, and redundant fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("classical-works");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::Work);

    REQUIRE(spec.sortBy.size() == 8);
    CHECK(spec.sortBy[0].field == TrackSortField::Composer);
    CHECK(spec.sortBy[1].field == TrackSortField::Work);
    CHECK(spec.sortBy[2].field == TrackSortField::Year);
    CHECK(spec.sortBy[3].field == TrackSortField::Album);
    CHECK(spec.sortBy[4].field == TrackSortField::Movement);
    CHECK(spec.sortBy[5].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[6].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[7].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 6);
    CHECK(spec.visibleFields[0] == TrackField::DisplayTrackNumber);
    CHECK(spec.visibleFields[1] == TrackField::Movement);
    CHECK(spec.visibleFields[2] == TrackField::Artist);
    CHECK(spec.visibleFields[3] == TrackField::Album);
    CHECK(spec.visibleFields[4] == TrackField::Year);
    CHECK(spec.visibleFields[5] == TrackField::Duration);

    REQUIRE(spec.redundantFields.size() == 2);
    CHECK(spec.redundantFields[0] == TrackField::Composer);
    CHECK(spec.redundantFields[1] == TrackField::Work);
  }

  TEST_CASE("tagging preset has all curation columns visible", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("tagging");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    CHECK(spec.groupBy == TrackGroupKey::None);

    // Curation view exposes raw disc/track numbers rather than the formatted
    // DisplayTrackNumber, so tagging mistakes (missing disc, wrong totals) are visible.
    REQUIRE(spec.visibleFields.size() == 9);
    CHECK(spec.visibleFields[0] == TrackField::DiscNumber);
    CHECK(spec.visibleFields[1] == TrackField::TrackNumber);
    CHECK(spec.visibleFields[2] == TrackField::Title);
    CHECK(spec.visibleFields[3] == TrackField::Artist);
    CHECK(spec.visibleFields[4] == TrackField::Album);
    CHECK(spec.visibleFields[5] == TrackField::Genre);
    CHECK(spec.visibleFields[6] == TrackField::Year);
    CHECK(spec.visibleFields[7] == TrackField::Duration);
    CHECK(spec.visibleFields[8] == TrackField::Tags);
    CHECK_FALSE(std::ranges::contains(spec.visibleFields, TrackField::DisplayTrackNumber));

    CHECK(spec.redundantFields.empty());
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

  TEST_CASE("trackFieldId round-trips through trackFieldFromId", "[runtime][unit][presentation]")
  {
    auto const fields = {
      TrackField::Title,
      TrackField::Artist,
      TrackField::Album,
      TrackField::AlbumArtist,
      TrackField::Genre,
      TrackField::Composer,
      TrackField::Work,
      TrackField::Year,
      TrackField::DiscNumber,
      TrackField::TrackNumber,
      TrackField::Duration,
      TrackField::Tags,
    };

    for (auto const field : fields)
    {
      auto const id = trackFieldId(field);
      auto const optParsed = trackFieldFromId(id);

      REQUIRE(optParsed.has_value());
      CHECK(*optParsed == field);
    }
  }

  TEST_CASE("trackFieldFromId returns nullopt for unknown id", "[runtime][unit][presentation]")
  {
    CHECK(!trackFieldFromId("bpm").has_value());
    CHECK(!trackFieldFromId("").has_value());
    CHECK(!trackFieldFromId("unknown-field").has_value());
  }
} // namespace ao::rt::test
