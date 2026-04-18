// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TrackPresentation.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
  [[nodiscard]] auto compareCaseInsensitive(std::string_view lhs, std::string_view rhs) -> int
  {
    auto const common = std::min(lhs.size(), rhs.size());

    for (std::size_t i = 0; i < common; ++i)
    {
      auto const lc = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(lhs[i])));
      auto const rc = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(rhs[i])));

      if (lc < rc)
      {
        return -1;
      }
      if (lc > rc)
      {
        return 1;
      }
    }

    if (lhs.size() < rhs.size())
    {
      return -1;
    }
    if (lhs.size() > rhs.size())
    {
      return 1;
    }

    return 0;
  }

  [[nodiscard]] auto compareTextField(std::string_view lhs, std::string_view rhs) -> int
  {
    auto const leftEmpty = lhs.empty();
    auto const rightEmpty = rhs.empty();

    if (leftEmpty != rightEmpty)
    {
      return leftEmpty ? 1 : -1;
    }

    if (auto const cmp = compareCaseInsensitive(lhs, rhs); cmp != 0)
    {
      return cmp;
    }

    if (lhs < rhs)
    {
      return -1;
    }
    if (lhs > rhs)
    {
      return 1;
    }

    return 0;
  }

  [[nodiscard]] auto compareNumberField(std::uint16_t lhs, std::uint16_t rhs) -> int
  {
    auto const leftUnknown = lhs == 0;
    auto const rightUnknown = rhs == 0;

    if (leftUnknown != rightUnknown)
    {
      return leftUnknown ? 1 : -1;
    }

    if (lhs < rhs)
    {
      return -1;
    }
    if (lhs > rhs)
    {
      return 1;
    }

    return 0;
  }

  [[nodiscard]] auto compareTrackId(rs::core::TrackId lhs, rs::core::TrackId rhs) -> int
  {
    if (lhs.value() < rhs.value())
    {
      return -1;
    }
    if (lhs.value() > rhs.value())
    {
      return 1;
    }

    return 0;
  }

  [[nodiscard]] auto compareByField(TrackPresentationKeysView lhs, TrackPresentationKeysView rhs, TrackSortField field)
    -> int
  {
    switch (field)
    {
      case TrackSortField::Artist: return compareTextField(lhs.artist, rhs.artist);
      case TrackSortField::Album: return compareTextField(lhs.album, rhs.album);
      case TrackSortField::AlbumArtist: return compareTextField(lhs.albumArtist, rhs.albumArtist);
      case TrackSortField::Genre: return compareTextField(lhs.genre, rhs.genre);
      case TrackSortField::Year: return compareNumberField(lhs.year, rhs.year);
      case TrackSortField::DiscNumber: return compareNumberField(lhs.discNumber, rhs.discNumber);
      case TrackSortField::TrackNumber: return compareNumberField(lhs.trackNumber, rhs.trackNumber);
      case TrackSortField::Title: return compareTextField(lhs.title, rhs.title);
    }

    return 0;
  }

  [[nodiscard]] auto unknownLabel(char const* label) -> std::string
  {
    return std::string{"Unknown "} + label;
  }
}

TrackPresentationSpec presentationSpecForGroup(TrackGroupBy groupBy)
{
  auto spec = TrackPresentationSpec{};
  spec.groupBy = groupBy;

  switch (groupBy)
  {
    case TrackGroupBy::None: return spec;
    case TrackGroupBy::Artist:
      spec.sortBy = {
        {TrackSortField::Artist},
        {TrackSortField::Album},
        {TrackSortField::DiscNumber},
        {TrackSortField::TrackNumber},
        {TrackSortField::Title},
      };
      return spec;
    case TrackGroupBy::Album:
      spec.sortBy = {
        {TrackSortField::AlbumArtist},
        {TrackSortField::Album},
        {TrackSortField::DiscNumber},
        {TrackSortField::TrackNumber},
        {TrackSortField::Title},
      };
      return spec;
    case TrackGroupBy::AlbumArtist:
      spec.sortBy = {
        {TrackSortField::AlbumArtist},
        {TrackSortField::Album},
        {TrackSortField::DiscNumber},
        {TrackSortField::TrackNumber},
        {TrackSortField::Title},
      };
      return spec;
    case TrackGroupBy::Genre:
      spec.sortBy = {
        {TrackSortField::Genre},
        {TrackSortField::Artist},
        {TrackSortField::Album},
        {TrackSortField::DiscNumber},
        {TrackSortField::TrackNumber},
        {TrackSortField::Title},
      };
      return spec;
    case TrackGroupBy::Year:
      spec.sortBy = {
        {TrackSortField::Year},
        {TrackSortField::Artist},
        {TrackSortField::Album},
        {TrackSortField::DiscNumber},
        {TrackSortField::TrackNumber},
        {TrackSortField::Title},
      };
      return spec;
  }

  return spec;
}

int compareForSort(TrackPresentationKeysView lhs, TrackPresentationKeysView rhs, std::span<TrackSortTerm const> sortBy)
{
  for (auto const& term : sortBy)
  {
    if (auto const cmp = compareByField(lhs, rhs, term.field); cmp != 0)
    {
      return cmp;
    }
  }

  return compareTrackId(lhs.trackId, rhs.trackId);
}

int compareForGrouping(TrackPresentationKeysView lhs, TrackPresentationKeysView rhs, TrackGroupBy groupBy)
{
  switch (groupBy)
  {
    case TrackGroupBy::None: return 0;
    case TrackGroupBy::Artist: return compareTextField(lhs.artist, rhs.artist);
    case TrackGroupBy::Album:
      if (auto const albumArtistCmp = compareTextField(lhs.albumArtist, rhs.albumArtist); albumArtistCmp != 0)
      {
        return albumArtistCmp;
      }
      return compareTextField(lhs.album, rhs.album);
    case TrackGroupBy::AlbumArtist: return compareTextField(lhs.albumArtist, rhs.albumArtist);
    case TrackGroupBy::Genre: return compareTextField(lhs.genre, rhs.genre);
    case TrackGroupBy::Year: return compareNumberField(lhs.year, rhs.year);
  }

  return 0;
}

bool shouldShowColumn(TrackGroupBy groupBy, TrackColumn column)
{
  switch (column)
  {
    case TrackColumn::Artist:
      return groupBy != TrackGroupBy::Artist && groupBy != TrackGroupBy::Album;
    case TrackColumn::Album:
      return groupBy != TrackGroupBy::Album;
    case TrackColumn::DiscNumber:
    case TrackColumn::TrackNumber:
    case TrackColumn::Title:
    case TrackColumn::Tags:
      return true;
  }

  return true;
}

std::string groupLabelFor(TrackPresentationKeysView keys, TrackGroupBy groupBy)
{
  switch (groupBy)
  {
    case TrackGroupBy::None: return {};
    case TrackGroupBy::Artist: return keys.artist.empty() ? unknownLabel("Artist") : std::string{keys.artist};
    case TrackGroupBy::Album:
      if (keys.album.empty())
      {
        return unknownLabel("Album");
      }

      if (keys.albumArtist.empty())
      {
        return std::string{keys.album};
      }

      return std::string{keys.album} + " - " + std::string{keys.albumArtist};
    case TrackGroupBy::AlbumArtist:
      return keys.albumArtist.empty() ? unknownLabel("Album Artist") : std::string{keys.albumArtist};
    case TrackGroupBy::Genre: return keys.genre.empty() ? unknownLabel("Genre") : std::string{keys.genre};
    case TrackGroupBy::Year: return keys.year == 0 ? unknownLabel("Year") : std::to_string(keys.year);
  }

  return {};
}
