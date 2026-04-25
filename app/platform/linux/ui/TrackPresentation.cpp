// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/TrackPresentation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace app::ui
{

  namespace
  {
    constexpr auto kTrackColumnDefinitions = std::to_array<TrackColumnDefinition>({
      {TrackColumn::Artist, "artist", "Artist", 150, true, false, false, false},
      {TrackColumn::Album, "album", "Album", 200, true, false, false, false},
      {TrackColumn::TrackNumber, "track-number", "Track", 72, true, false, true, false},
      {TrackColumn::Title, "title", "Title", -1, true, false, false, false},
      {TrackColumn::Duration, "duration", "Duration", 84, true, false, true, false},
      {TrackColumn::Tags, "tags", "Tags", -1, true, true, false, true},
      {TrackColumn::AlbumArtist, "album-artist", "Album Artist", 180, false, false, false, false},
      {TrackColumn::Genre, "genre", "Genre", 140, false, false, false, false},
      {TrackColumn::Composer, "composer", "Composer", 140, false, false, false, false},
      {TrackColumn::Work, "work", "Work", 140, false, false, false, false},
      {TrackColumn::Year, "year", "Year", 80, false, false, true, false},
      {TrackColumn::DiscNumber, "disc-number", "Disc", 70, false, false, true, false},
    });

    TrackColumnDefinition const* trackColumnDefinition(TrackColumn column)
    {
      auto const it = std::ranges::find(kTrackColumnDefinitions, column, &TrackColumnDefinition::column);
      
      if (it == kTrackColumnDefinitions.end())
      {
        return nullptr;
      }

      return &*it;
    }

    int compareCaseInsensitive(std::string_view lhs, std::string_view rhs)
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

    int compareTextField(std::string_view lhs, std::string_view rhs)
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

    template<typename T>
    int compareNumberField(T lhs, T rhs)
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

    int compareTrackId(rs::core::TrackId lhs, rs::core::TrackId rhs)
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

    int compareByField(TrackPresentationKeysView lhs, TrackPresentationKeysView rhs, TrackSortField field)
    {
      switch (field)
      {
        case TrackSortField::Artist: return compareTextField(lhs.artist, rhs.artist);
        case TrackSortField::Album: return compareTextField(lhs.album, rhs.album);
        case TrackSortField::AlbumArtist: return compareTextField(lhs.albumArtist, rhs.albumArtist);
        case TrackSortField::Genre: return compareTextField(lhs.genre, rhs.genre);
        case TrackSortField::Composer: return compareTextField(lhs.composer, rhs.composer);
        case TrackSortField::Work: return compareTextField(lhs.work, rhs.work);
        case TrackSortField::Year: return compareNumberField(lhs.year, rhs.year);
        case TrackSortField::DiscNumber: return compareNumberField(lhs.discNumber, rhs.discNumber);
        case TrackSortField::TrackNumber: return compareNumberField(lhs.trackNumber, rhs.trackNumber);
        case TrackSortField::Title: return compareTextField(lhs.title, rhs.title);
        case TrackSortField::Duration: return compareNumberField(lhs.durationMs, rhs.durationMs);
      }

      return 0;
    }

    std::string unknownLabel(char const* label)
    {
      return std::string{"Unknown "} + label;
    }

    TrackColumnState defaultColumnState(TrackColumn column)
    {
      auto const* definition = trackColumnDefinition(column);
      
      if (!definition)
      {
        return {.column = column};
      }

      return {
        .column = definition->column,
        .visible = definition->defaultVisible,
        .width = definition->defaultWidth,
      };
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
      case TrackGroupBy::Composer:
        spec.sortBy = {
          {TrackSortField::Composer},
          {TrackSortField::Artist},
          {TrackSortField::Album},
          {TrackSortField::DiscNumber},
          {TrackSortField::TrackNumber},
          {TrackSortField::Title},
        };
        return spec;
      case TrackGroupBy::Work:
        spec.sortBy = {
          {TrackSortField::Work},
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

  int compareForSort(TrackPresentationKeysView lhs,
                     TrackPresentationKeysView rhs,
                     std::span<TrackSortTerm const> sortBy)
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
      case TrackGroupBy::Composer: return compareTextField(lhs.composer, rhs.composer);
      case TrackGroupBy::Work: return compareTextField(lhs.work, rhs.work);
      case TrackGroupBy::Year: return compareNumberField(lhs.year, rhs.year);
    }

    return 0;
  }

  bool shouldShowColumn(TrackGroupBy groupBy, TrackColumn column)
  {
    switch (column)
    {
      case TrackColumn::Artist: return groupBy != TrackGroupBy::Artist && groupBy != TrackGroupBy::Album;
      case TrackColumn::Album: return groupBy != TrackGroupBy::Album;
      case TrackColumn::AlbumArtist: return groupBy != TrackGroupBy::Album && groupBy != TrackGroupBy::AlbumArtist;
      case TrackColumn::Genre: return groupBy != TrackGroupBy::Genre;
      case TrackColumn::Composer: return groupBy != TrackGroupBy::Composer;
      case TrackColumn::Work: return groupBy != TrackGroupBy::Work;
      case TrackColumn::Year: return groupBy != TrackGroupBy::Year;
      case TrackColumn::DiscNumber:
      case TrackColumn::TrackNumber:
      case TrackColumn::Title:
      case TrackColumn::Duration:
      case TrackColumn::Tags: return true;
    }

    return true;
  }

  std::span<TrackColumnDefinition const> trackColumnDefinitions()
  {
    return kTrackColumnDefinitions;
  }

  std::optional<TrackColumn> trackColumnFromId(std::string_view id)
  {
    auto const it = std::ranges::find(kTrackColumnDefinitions, id, &TrackColumnDefinition::id);
    
    if (it == kTrackColumnDefinitions.end())
    {
      return std::nullopt;
    }

    return it->column;
  }

  std::string_view trackColumnId(TrackColumn column)
  {
    auto const* definition = trackColumnDefinition(column);
    return definition ? definition->id : std::string_view{};
  }

  TrackColumnLayout defaultTrackColumnLayout()
  {
    auto layout = TrackColumnLayout{};
    layout.columns.reserve(kTrackColumnDefinitions.size());

    for (auto const& definition : kTrackColumnDefinitions)
    {
      layout.columns.push_back(defaultColumnState(definition.column));
    }

    return layout;
  }

  TrackColumnLayout normalizeTrackColumnLayout(TrackColumnLayout layout)
  {
    auto result = TrackColumnLayout{};
    result.columns.reserve(kTrackColumnDefinitions.size());

    auto hasColumn = [&result](TrackColumn column)
    { return std::ranges::find(result.columns, column, &TrackColumnState::column) != result.columns.end(); };

    for (auto const& state : layout.columns)
    {
      auto const* definition = trackColumnDefinition(state.column);
      
      if (!definition || hasColumn(state.column))
      {
        continue;
      }

      result.columns.push_back({
        .column = state.column,
        .visible = state.visible,
        .width = state.width < -1 ? definition->defaultWidth : state.width,
      });
    }

    for (auto const& definition : kTrackColumnDefinitions)
    {
      if (!hasColumn(definition.column))
      {
        result.columns.push_back(defaultColumnState(definition.column));
      }
    }

    return result;
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
      case TrackGroupBy::Composer: return keys.composer.empty() ? unknownLabel("Composer") : std::string{keys.composer};
      case TrackGroupBy::Work: return keys.work.empty() ? unknownLabel("Work") : std::string{keys.work};
      case TrackGroupBy::Year: return keys.year == 0 ? unknownLabel("Year") : std::to_string(keys.year);
    }

    return {};
  }

  TrackColumnLayoutModel::TrackColumnLayoutModel(TrackColumnLayout layout)
    : _layout{normalizeTrackColumnLayout(std::move(layout))}
  {
  }

  void TrackColumnLayoutModel::setLayout(TrackColumnLayout layout)
  {
    auto normalized = normalizeTrackColumnLayout(std::move(layout));

    if (_layout == normalized)
    {
      return;
    }

    _layout = std::move(normalized);
    _changed.emit();
  }

  void TrackColumnLayoutModel::reset()
  {
    setLayout(defaultTrackColumnLayout());
  }

} // namespace app::ui
