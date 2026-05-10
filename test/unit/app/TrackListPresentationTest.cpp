// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <runtime/TrackListPresentation.h>

using namespace ao::app;

TEST_CASE("TrackListPresentation: None mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::None);
  REQUIRE(snapshot.groupBy == TrackGroupKey::None);
  REQUIRE(snapshot.effectiveSortBy.empty());
  REQUIRE(snapshot.redundantFields.empty());
}

TEST_CASE("TrackListPresentation: Artist mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::Artist);
  REQUIRE(snapshot.groupBy == TrackGroupKey::Artist);

  std::vector<TrackSortField> expectedSort = {TrackSortField::Artist,
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

  std::vector<TrackSortField> expectedRedundant = {TrackSortField::Artist};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}

TEST_CASE("TrackListPresentation: Album mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::Album);
  REQUIRE(snapshot.groupBy == TrackGroupKey::Album);

  std::vector<TrackSortField> expectedSort = {TrackSortField::AlbumArtist,
                                              TrackSortField::Album,
                                              TrackSortField::DiscNumber,
                                              TrackSortField::TrackNumber,
                                              TrackSortField::Title};
  REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());
  for (size_t i = 0; i < expectedSort.size(); ++i)
  {
    REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
  }

  std::vector<TrackSortField> expectedRedundant = {
    TrackSortField::Artist, TrackSortField::Album, TrackSortField::AlbumArtist};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}

TEST_CASE("TrackListPresentation: AlbumArtist mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::AlbumArtist);
  REQUIRE(snapshot.groupBy == TrackGroupKey::AlbumArtist);

  std::vector<TrackSortField> expectedSort = {TrackSortField::AlbumArtist,
                                              TrackSortField::Album,
                                              TrackSortField::DiscNumber,
                                              TrackSortField::TrackNumber,
                                              TrackSortField::Title};
  REQUIRE(snapshot.effectiveSortBy.size() == expectedSort.size());
  for (size_t i = 0; i < expectedSort.size(); ++i)
  {
    REQUIRE(snapshot.effectiveSortBy[i].field == expectedSort[i]);
  }

  std::vector<TrackSortField> expectedRedundant = {TrackSortField::AlbumArtist};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}

TEST_CASE("TrackListPresentation: Genre mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::Genre);
  REQUIRE(snapshot.groupBy == TrackGroupKey::Genre);

  std::vector<TrackSortField> expectedSort = {TrackSortField::Genre,
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

  std::vector<TrackSortField> expectedRedundant = {TrackSortField::Genre};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}

TEST_CASE("TrackListPresentation: Composer mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::Composer);
  REQUIRE(snapshot.groupBy == TrackGroupKey::Composer);

  std::vector<TrackSortField> expectedSort = {TrackSortField::Composer,
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

  std::vector<TrackSortField> expectedRedundant = {TrackSortField::Composer};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}

TEST_CASE("TrackListPresentation: Work mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::Work);
  REQUIRE(snapshot.groupBy == TrackGroupKey::Work);

  std::vector<TrackSortField> expectedSort = {TrackSortField::Work,
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

  std::vector<TrackSortField> expectedRedundant = {TrackSortField::Work};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}

TEST_CASE("TrackListPresentation: Year mapping", "[app][runtime][presentation]")
{
  auto snapshot = presentationForGroup(TrackGroupKey::Year);
  REQUIRE(snapshot.groupBy == TrackGroupKey::Year);

  std::vector<TrackSortField> expectedSort = {TrackSortField::Year,
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

  std::vector<TrackSortField> expectedRedundant = {TrackSortField::Year};
  REQUIRE(snapshot.redundantFields == expectedRedundant);
}
