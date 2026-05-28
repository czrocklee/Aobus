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

    CHECK(findPreset("songs"));
    CHECK(findPreset("albums"));
    CHECK(findPreset("artists"));
    CHECK(findPreset("album-artists"));
    CHECK(findPreset("classical-composers"));
    CHECK(findPreset("classical-works"));
    CHECK(findPreset("genres"));
    CHECK(findPreset("years"));
    CHECK(findPreset("tagging"));
  }

  TEST_CASE("builtinTrackPresentationPreset lookup by id", "[runtime][unit][presentation]")
  {
    CHECK(builtinTrackPresentationPreset("songs") != nullptr);
    CHECK(builtinTrackPresentationPreset("albums") != nullptr);
    CHECK(builtinTrackPresentationPreset("nonexistent") == nullptr);
  }

  TEST_CASE("defaultTrackPresentationSpec is songs", "[runtime][unit][presentation]")
  {
    auto spec = defaultTrackPresentationSpec();

    REQUIRE(spec.id == "songs");
    REQUIRE(spec.groupBy == TrackGroupKey::None);
    REQUIRE(spec.redundantFields.empty());
  }

  TEST_CASE("songs preset has correct sort and visible fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("songs");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    REQUIRE(spec.groupBy == TrackGroupKey::None);

    REQUIRE(spec.sortBy.size() == 5);
    CHECK(spec.sortBy[0].field == TrackSortField::Artist);
    CHECK(spec.sortBy[1].field == TrackSortField::Album);
    CHECK(spec.sortBy[2].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[3].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[4].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 5);
    CHECK(spec.visibleFields[0] == TrackField::Title);
    CHECK(spec.visibleFields[1] == TrackField::Artist);
    CHECK(spec.visibleFields[2] == TrackField::Album);
    CHECK(spec.visibleFields[3] == TrackField::Duration);
    CHECK(spec.visibleFields[4] == TrackField::Tags);

    CHECK(spec.redundantFields.empty());
  }

  TEST_CASE("albums preset has correct group, sort, and redundant fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    REQUIRE(spec.groupBy == TrackGroupKey::Album);

    REQUIRE(spec.sortBy.size() == 5);
    CHECK(spec.sortBy[0].field == TrackSortField::AlbumArtist);
    CHECK(spec.sortBy[1].field == TrackSortField::Album);
    CHECK(spec.sortBy[2].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[3].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[4].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 5);
    CHECK(spec.visibleFields[0] == TrackField::TrackNumber);
    CHECK(spec.visibleFields[1] == TrackField::Title);
    CHECK(spec.visibleFields[2] == TrackField::Duration);
    CHECK(spec.visibleFields[3] == TrackField::Year);
    CHECK(spec.visibleFields[4] == TrackField::Tags);

    REQUIRE(spec.redundantFields.size() == 2);
    CHECK(spec.redundantFields[0] == TrackField::Album);
    CHECK(spec.redundantFields[1] == TrackField::AlbumArtist);
  }

  TEST_CASE("classical-composers preset has correct sort and visible fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("classical-composers");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    REQUIRE(spec.groupBy == TrackGroupKey::Composer);

    REQUIRE(spec.sortBy.size() == 6);
    CHECK(spec.sortBy[0].field == TrackSortField::Composer);
    CHECK(spec.sortBy[1].field == TrackSortField::Work);
    CHECK(spec.sortBy[2].field == TrackSortField::Album);
    CHECK(spec.sortBy[3].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[4].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[5].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 6);
    CHECK(spec.visibleFields[0] == TrackField::Work);
    CHECK(spec.visibleFields[1] == TrackField::Title);
    CHECK(spec.visibleFields[2] == TrackField::Artist);
    CHECK(spec.visibleFields[3] == TrackField::Album);
    CHECK(spec.visibleFields[4] == TrackField::Duration);
    CHECK(spec.visibleFields[5] == TrackField::Year);

    REQUIRE(spec.redundantFields.size() == 1);
    CHECK(spec.redundantFields[0] == TrackField::Composer);
  }

  TEST_CASE("classical-works preset has correct group, sort, and redundant fields", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("classical-works");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    REQUIRE(spec.groupBy == TrackGroupKey::Work);

    REQUIRE(spec.sortBy.size() == 5);
    CHECK(spec.sortBy[0].field == TrackSortField::Composer);
    CHECK(spec.sortBy[1].field == TrackSortField::Work);
    CHECK(spec.sortBy[2].field == TrackSortField::DiscNumber);
    CHECK(spec.sortBy[3].field == TrackSortField::TrackNumber);
    CHECK(spec.sortBy[4].field == TrackSortField::Title);

    REQUIRE(spec.visibleFields.size() == 5);
    CHECK(spec.visibleFields[0] == TrackField::Composer);
    CHECK(spec.visibleFields[1] == TrackField::Title);
    CHECK(spec.visibleFields[2] == TrackField::Artist);
    CHECK(spec.visibleFields[3] == TrackField::Album);
    CHECK(spec.visibleFields[4] == TrackField::Duration);

    REQUIRE(spec.redundantFields.size() == 1);
    CHECK(spec.redundantFields[0] == TrackField::Work);
  }

  TEST_CASE("tagging preset has all curation columns visible", "[runtime][unit][presentation]")
  {
    auto const* preset = builtinTrackPresentationPreset("tagging");
    REQUIRE(preset != nullptr);

    auto const& spec = preset->spec;

    REQUIRE(spec.groupBy == TrackGroupKey::None);

    REQUIRE(spec.visibleFields.size() == 6);
    CHECK(spec.visibleFields[0] == TrackField::Title);
    CHECK(spec.visibleFields[1] == TrackField::Artist);
    CHECK(spec.visibleFields[2] == TrackField::Album);
    CHECK(spec.visibleFields[3] == TrackField::Genre);
    CHECK(spec.visibleFields[4] == TrackField::Year);
    CHECK(spec.visibleFields[5] == TrackField::Tags);

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

  TEST_CASE("normalizeTrackPresentationSpec defaults empty id to songs", "[runtime][unit][presentation]")
  {
    auto spec = TrackPresentationSpec{.id = ""};

    auto normalized = normalizeTrackPresentationSpec(spec);

    CHECK(normalized.id == "songs");
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