// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackPresentation.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kTrackColumnDefinitions = std::to_array<TrackColumnDefinition>({
      {
        .column = TrackColumn::Artist,
        .id = "artist",
        .title = "Artist",
        .defaultWidth = 150,
        .editable = true,
        .draggable = true,
      },
      {
        .column = TrackColumn::Album,
        .id = "album",
        .title = "Album",
        .defaultWidth = 200,
        .editable = true,
        .draggable = true,
      },
      {
        .column = TrackColumn::TrackNumber,
        .id = "track-number",
        .title = "Track",
        .defaultWidth = 72,
        .numeric = true,
      },
      {
        .column = TrackColumn::Title,
        .id = "title",
        .title = "Title",
        .editable = true,
      },
      {
        .column = TrackColumn::Duration,
        .id = "duration",
        .title = "Duration",
        .defaultWidth = 84,
        .numeric = true,
      },
      {
        .column = TrackColumn::Tags,
        .id = "tags",
        .title = "Tags",
        .expands = true,
        .tagsCell = true,
      },
      {
        .column = TrackColumn::AlbumArtist,
        .id = "album-artist",
        .title = "Album Artist",
        .defaultWidth = 180,
        .defaultVisible = false,
      },
      {
        .column = TrackColumn::Genre,
        .id = "genre",
        .title = "Genre",
        .defaultWidth = 140,
        .defaultVisible = false,
        .draggable = true,
      },
      {
        .column = TrackColumn::Composer,
        .id = "composer",
        .title = "Composer",
        .defaultWidth = 140,
        .defaultVisible = false,
      },
      {
        .column = TrackColumn::Work,
        .id = "work",
        .title = "Work",
        .defaultWidth = 140,
        .defaultVisible = false,
      },
      {
        .column = TrackColumn::Year,
        .id = "year",
        .title = "Year",
        .defaultWidth = 80,
        .defaultVisible = false,
        .numeric = true,
      },
      {
        .column = TrackColumn::DiscNumber,
        .id = "disc-number",
        .title = "Disc",
        .defaultWidth = 70,
        .defaultVisible = false,
        .numeric = true,
      },
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

  std::optional<TrackColumn> redundantFieldToColumn(rt::TrackSortField field)
  {
    switch (field)
    {
      case rt::TrackSortField::Artist: return TrackColumn::Artist;
      case rt::TrackSortField::Album: return TrackColumn::Album;
      case rt::TrackSortField::AlbumArtist: return TrackColumn::AlbumArtist;
      case rt::TrackSortField::Genre: return TrackColumn::Genre;
      case rt::TrackSortField::Composer: return TrackColumn::Composer;
      case rt::TrackSortField::Work: return TrackColumn::Work;
      case rt::TrackSortField::Year: return TrackColumn::Year;
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
