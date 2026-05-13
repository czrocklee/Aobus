// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/TrackListPresentation.h>

namespace ao::rt::test
{
  TEST_CASE("TrackListPresentation: None mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::None);
    REQUIRE(snapshot.groupBy == TrackGroupKey::None);

    auto const expectedSort = std::vector<TrackSortField>{TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
      REQUIRE(snapshot.effectiveSortBy[i].ascending == true);
    }

    REQUIRE(snapshot.redundantFields.empty());
  }

  TEST_CASE("TrackListPresentation: Artist mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::Artist);
    REQUIRE(snapshot.groupBy == TrackGroupKey::Artist);

    auto const expectedSort = std::vector<TrackSortField>{TrackSortField::Artist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
      REQUIRE(snapshot.effectiveSortBy[i].ascending == true);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {TrackPresentationField::Artist};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Album mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::Album);
    REQUIRE(snapshot.groupBy == TrackGroupKey::Album);

    auto const expectedSort = std::vector<TrackSortField>{TrackSortField::AlbumArtist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {
      TrackPresentationField::Artist, TrackPresentationField::Album, TrackPresentationField::AlbumArtist};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: AlbumArtist mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::AlbumArtist);
    REQUIRE(snapshot.groupBy == TrackGroupKey::AlbumArtist);

    auto const expectedSort = std::vector<TrackSortField>{TrackSortField::AlbumArtist,
                                                          TrackSortField::Album,
                                                          TrackSortField::DiscNumber,
                                                          TrackSortField::TrackNumber,
                                                          TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {TrackPresentationField::AlbumArtist};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Genre mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::Genre);
    REQUIRE(snapshot.groupBy == TrackGroupKey::Genre);

    std::vector<TrackSortField> const expectedSort = {TrackSortField::Genre,
                                                      TrackSortField::Artist,
                                                      TrackSortField::Album,
                                                      TrackSortField::DiscNumber,
                                                      TrackSortField::TrackNumber,
                                                      TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {TrackPresentationField::Genre};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Composer mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::Composer);
    REQUIRE(snapshot.groupBy == TrackGroupKey::Composer);

    std::vector<TrackSortField> const expectedSort = {TrackSortField::Composer,
                                                      TrackSortField::Artist,
                                                      TrackSortField::Album,
                                                      TrackSortField::DiscNumber,
                                                      TrackSortField::TrackNumber,
                                                      TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {TrackPresentationField::Composer};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Work mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::Work);
    REQUIRE(snapshot.groupBy == TrackGroupKey::Work);

    std::vector<TrackSortField> const expectedSort = {TrackSortField::Work,
                                                      TrackSortField::Artist,
                                                      TrackSortField::Album,
                                                      TrackSortField::DiscNumber,
                                                      TrackSortField::TrackNumber,
                                                      TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {TrackPresentationField::Work};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }

  TEST_CASE("TrackListPresentation: Year mapping", "[app][runtime][presentation]")
  {
    auto const snapshot = presentationForGroup(TrackGroupKey::Year);
    REQUIRE(snapshot.groupBy == TrackGroupKey::Year);

    std::vector<TrackSortField> const expectedSort = {TrackSortField::Year,
                                                      TrackSortField::Artist,
                                                      TrackSortField::Album,
                                                      TrackSortField::DiscNumber,
                                                      TrackSortField::TrackNumber,
                                                      TrackSortField::Title};
    REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());

    for (size_t i = 0; i < expectedSort.size(); ++i)
    {
      REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
    }

    std::vector<TrackPresentationField> const expectedRedundant = {TrackPresentationField::Year};
    REQUIRE(snapshot.redundantFields == expectedRedundant);
  }
} // namespace ao::rt::test
