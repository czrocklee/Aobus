// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "TrackPresentation.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>

namespace ao::gtk
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
      auto const* const it = std::ranges::find(kTrackColumnDefinitions, column, &TrackColumnDefinition::column);

      if (it == kTrackColumnDefinitions.end())
      {
        return nullptr;
      }

      return &*it;
    }

    TrackColumnState defaultColumnState(TrackColumn column)
    {
      auto const* definition = trackColumnDefinition(column);

      if (definition == nullptr)
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

  std::optional<TrackColumn> redundantFieldToColumn(ao::rt::TrackSortField field)
  {
    switch (field)
    {
      case ao::rt::TrackSortField::Artist: return TrackColumn::Artist;
      case ao::rt::TrackSortField::Album: return TrackColumn::Album;
      case ao::rt::TrackSortField::AlbumArtist: return TrackColumn::AlbumArtist;
      case ao::rt::TrackSortField::Genre: return TrackColumn::Genre;
      case ao::rt::TrackSortField::Composer: return TrackColumn::Composer;
      case ao::rt::TrackSortField::Work: return TrackColumn::Work;
      case ao::rt::TrackSortField::Year: return TrackColumn::Year;
      default: return std::nullopt;
    }
  }

  std::span<TrackColumnDefinition const> trackColumnDefinitions()
  {
    return kTrackColumnDefinitions;
  }

  std::optional<TrackColumn> trackColumnFromId(std::string_view id)
  {
    auto const* const it = std::ranges::find(kTrackColumnDefinitions, id, &TrackColumnDefinition::id);

    if (it == kTrackColumnDefinitions.end())
    {
      return std::nullopt;
    }

    return it->column;
  }

  std::string_view trackColumnId(TrackColumn column)
  {
    auto const* definition = trackColumnDefinition(column);
    return definition != nullptr ? definition->id : std::string_view{};
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

  TrackColumnLayout normalizeTrackColumnLayout(TrackColumnLayout const& layout)
  {
    auto result = TrackColumnLayout{};
    result.columns.reserve(kTrackColumnDefinitions.size());

    auto hasColumn = [&result](TrackColumn column)
    { return std::ranges::contains(result.columns, column, &TrackColumnState::column); };

    for (auto const& state : layout.columns)
    {
      auto const* definition = trackColumnDefinition(state.column);

      if (definition == nullptr || hasColumn(state.column))
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

  TrackColumnLayoutModel::TrackColumnLayoutModel(TrackColumnLayout const& layout)
    : _layout{normalizeTrackColumnLayout(layout)}
  {
  }

  void TrackColumnLayoutModel::setLayout(TrackColumnLayout const& layout)
  {
    auto normalized = normalizeTrackColumnLayout(layout);

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
} // namespace ao::gtk
