// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <app/runtime/TrackPresentationPreset.h>
#include <runtime/StateTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    TrackPresentationPreset const* presetForGroup(TrackGroupKey key)
    {
      auto const presets = builtinTrackPresentationPresets();
      auto const iter =
        std::ranges::find(presets, key, [](TrackPresentationPreset const& p) { return p.spec.groupBy; });

      if (iter == presets.end())
      {
        return nullptr;
      }

      return &*iter;
    }
  } // namespace

  TEST_CASE("TrackListPresentation: None mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::None);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::None);

    auto const expectedSort = std::vector{TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
      REQUIRE(preset->spec.sortBy[i].ascending == true);
    }

    REQUIRE(preset->spec.redundantFields.empty());
  }

  TEST_CASE("TrackListPresentation: Artist mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::Artist);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::Artist);

    auto const expectedSort = std::vector{TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
      REQUIRE(preset->spec.sortBy[i].ascending == true);
    }

    auto const expectedRedundant = std::vector{TrackPresentationField::Artist};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Album mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::Album);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::Album);

    auto const expectedSort = std::vector{TrackSortField::AlbumArtist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
    }

    auto const expectedRedundant = std::vector{
      TrackPresentationField::Artist, TrackPresentationField::Album, TrackPresentationField::AlbumArtist};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: AlbumArtist mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::AlbumArtist);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::AlbumArtist);

    auto const expectedSort = std::vector{TrackSortField::AlbumArtist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
    }

    auto const expectedRedundant = std::vector{TrackPresentationField::AlbumArtist};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Genre mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::Genre);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::Genre);

    auto const expectedSort = std::vector{TrackSortField::Genre,
                                                          TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
    }

    auto const expectedRedundant = std::vector{TrackPresentationField::Genre};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Composer mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::Composer);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::Composer);

    auto const expectedSort = std::vector{TrackSortField::Composer,
                                                          TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
    }

    auto const expectedRedundant = std::vector{TrackPresentationField::Composer};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Work mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::Work);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::Work);

    auto const expectedSort = std::vector{TrackSortField::Work,
                                                          TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
    }

    auto const expectedRedundant = std::vector{TrackPresentationField::Work};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Year mapping", "[app][runtime][presentation]")
  {
    auto const* const preset = presetForGroup(TrackGroupKey::Year);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->spec.groupBy == TrackGroupKey::Year);

    auto const expectedSort = std::vector{TrackSortField::Year,
                                                          TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(preset->spec.sortBy.size() == expectedSort.size());

    for (std::size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(preset->spec.sortBy[i].field == expectedSort[i]);
    }

    auto const expectedRedundant = std::vector{TrackPresentationField::Year};
    REQUIRE(preset->spec.redundantFields == expectedRedundant);
  }
} // namespace ao::rt::test
