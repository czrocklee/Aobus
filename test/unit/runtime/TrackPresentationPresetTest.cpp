// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <runtime/StateTypes.h>
#include <runtime/TrackPresentationPreset.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

using namespace ao::rt;

TEST_CASE("builtinTrackPresentationPresets contains all expected ids", "[runtime][presentation]")
{
  auto presets = builtinTrackPresentationPresets();

  auto findPreset = [&](std::string_view id) -> bool
  {
    return std::ranges::find(presets, id, [](TrackPresentationPreset const& p) { return p.spec.id; }) != presets.end();
  };

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

TEST_CASE("builtinTrackPresentationPreset lookup by id", "[runtime][presentation]")
{
  CHECK(builtinTrackPresentationPreset("songs") != nullptr);
  CHECK(builtinTrackPresentationPreset("albums") != nullptr);
  CHECK(builtinTrackPresentationPreset("nonexistent") == nullptr);
}

TEST_CASE("defaultTrackPresentationSpec is songs", "[runtime][presentation]")
{
  auto spec = defaultTrackPresentationSpec();

  REQUIRE(spec.id == "songs");
  REQUIRE(spec.groupBy == TrackGroupKey::None);
  REQUIRE(spec.redundantFields.empty());
}

TEST_CASE("songs preset has correct sort and visible fields", "[runtime][presentation]")
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
  CHECK(spec.visibleFields[0] == TrackPresentationField::Title);
  CHECK(spec.visibleFields[1] == TrackPresentationField::Artist);
  CHECK(spec.visibleFields[2] == TrackPresentationField::Album);
  CHECK(spec.visibleFields[3] == TrackPresentationField::Duration);
  CHECK(spec.visibleFields[4] == TrackPresentationField::Tags);

  CHECK(spec.redundantFields.empty());
}

TEST_CASE("albums preset has correct group, sort, and redundant fields", "[runtime][presentation]")
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
  CHECK(spec.visibleFields[0] == TrackPresentationField::TrackNumber);
  CHECK(spec.visibleFields[1] == TrackPresentationField::Title);
  CHECK(spec.visibleFields[2] == TrackPresentationField::Duration);
  CHECK(spec.visibleFields[3] == TrackPresentationField::Year);
  CHECK(spec.visibleFields[4] == TrackPresentationField::Tags);

  REQUIRE(spec.redundantFields.size() == 2);
  CHECK(spec.redundantFields[0] == TrackPresentationField::Album);
  CHECK(spec.redundantFields[1] == TrackPresentationField::AlbumArtist);
}

TEST_CASE("classical-composers preset has correct sort and visible fields", "[runtime][presentation]")
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
  CHECK(spec.visibleFields[0] == TrackPresentationField::Work);
  CHECK(spec.visibleFields[1] == TrackPresentationField::Title);
  CHECK(spec.visibleFields[2] == TrackPresentationField::Artist);
  CHECK(spec.visibleFields[3] == TrackPresentationField::Album);
  CHECK(spec.visibleFields[4] == TrackPresentationField::Duration);
  CHECK(spec.visibleFields[5] == TrackPresentationField::Year);

  REQUIRE(spec.redundantFields.size() == 1);
  CHECK(spec.redundantFields[0] == TrackPresentationField::Composer);
}

TEST_CASE("classical-works preset has correct group, sort, and redundant fields", "[runtime][presentation]")
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
  CHECK(spec.visibleFields[0] == TrackPresentationField::Composer);
  CHECK(spec.visibleFields[1] == TrackPresentationField::Title);
  CHECK(spec.visibleFields[2] == TrackPresentationField::Artist);
  CHECK(spec.visibleFields[3] == TrackPresentationField::Album);
  CHECK(spec.visibleFields[4] == TrackPresentationField::Duration);

  REQUIRE(spec.redundantFields.size() == 1);
  CHECK(spec.redundantFields[0] == TrackPresentationField::Work);
}

TEST_CASE("tagging preset has all curation columns visible", "[runtime][presentation]")
{
  auto const* preset = builtinTrackPresentationPreset("tagging");
  REQUIRE(preset != nullptr);

  auto const& spec = preset->spec;

  REQUIRE(spec.groupBy == TrackGroupKey::None);

  REQUIRE(spec.visibleFields.size() == 6);
  CHECK(spec.visibleFields[0] == TrackPresentationField::Title);
  CHECK(spec.visibleFields[1] == TrackPresentationField::Artist);
  CHECK(spec.visibleFields[2] == TrackPresentationField::Album);
  CHECK(spec.visibleFields[3] == TrackPresentationField::Genre);
  CHECK(spec.visibleFields[4] == TrackPresentationField::Year);
  CHECK(spec.visibleFields[5] == TrackPresentationField::Tags);

  CHECK(spec.redundantFields.empty());
}

TEST_CASE("normalizeTrackPresentationSpec removes duplicate visible fields", "[runtime][presentation]")
{
  auto spec = TrackPresentationSpec{
    .id = "test",
    .visibleFields =
      {
        TrackPresentationField::Title,
        TrackPresentationField::Artist,
        TrackPresentationField::Title,
        TrackPresentationField::Album,
        TrackPresentationField::Artist,
      },
  };

  auto normalized = normalizeTrackPresentationSpec(spec);

  REQUIRE(normalized.visibleFields.size() == 3);
  CHECK(normalized.visibleFields[0] == TrackPresentationField::Title);
  CHECK(normalized.visibleFields[1] == TrackPresentationField::Artist);
  CHECK(normalized.visibleFields[2] == TrackPresentationField::Album);
}

TEST_CASE("normalizeTrackPresentationSpec removes duplicate redundant fields", "[runtime][presentation]")
{
  auto spec = TrackPresentationSpec{
    .id = "test",
    .redundantFields =
      {
        TrackPresentationField::Album,
        TrackPresentationField::AlbumArtist,
        TrackPresentationField::Album,
      },
  };

  auto normalized = normalizeTrackPresentationSpec(spec);

  REQUIRE(normalized.redundantFields.size() == 2);
  CHECK(normalized.redundantFields[0] == TrackPresentationField::Album);
  CHECK(normalized.redundantFields[1] == TrackPresentationField::AlbumArtist);
}

TEST_CASE("normalizeTrackPresentationSpec defaults empty id to songs", "[runtime][presentation]")
{
  auto spec = TrackPresentationSpec{.id = ""};

  auto normalized = normalizeTrackPresentationSpec(spec);

  CHECK(normalized.id == "songs");
}

TEST_CASE("trackPresentationFieldId round-trips through trackPresentationFieldFromId", "[runtime][presentation]")
{
  auto const fields = {
    TrackPresentationField::Title,
    TrackPresentationField::Artist,
    TrackPresentationField::Album,
    TrackPresentationField::AlbumArtist,
    TrackPresentationField::Genre,
    TrackPresentationField::Composer,
    TrackPresentationField::Work,
    TrackPresentationField::Year,
    TrackPresentationField::DiscNumber,
    TrackPresentationField::TrackNumber,
    TrackPresentationField::Duration,
    TrackPresentationField::Tags,
  };

  for (auto const field : fields)
  {
    auto const id = trackPresentationFieldId(field);
    auto const optParsed = trackPresentationFieldFromId(id);

    REQUIRE(optParsed.has_value());
    CHECK(*optParsed == field);
  }
}

TEST_CASE("trackPresentationFieldFromId returns nullopt for unknown id", "[runtime][presentation]")
{
  CHECK(!trackPresentationFieldFromId("bpm").has_value());
  CHECK(!trackPresentationFieldFromId("").has_value());
  CHECK(!trackPresentationFieldFromId("unknown-field").has_value());
}
