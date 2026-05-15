// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackPresentation.h"
#include <runtime/ProjectionTypes.h>
#include <runtime/StateTypes.h>
#include <runtime/TrackPresentationPreset.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

    TrackColumnState defaultColumnState(TrackColumn column)
    {
      return defaultTrackColumnState(column);
    }
  } // namespace

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

  std::optional<TrackColumn> trackColumnForPresentationField(rt::TrackPresentationField field)
  {
    switch (field)
    {
      case rt::TrackPresentationField::Title: return TrackColumn::Title;
      case rt::TrackPresentationField::Artist: return TrackColumn::Artist;
      case rt::TrackPresentationField::Album: return TrackColumn::Album;
      case rt::TrackPresentationField::AlbumArtist: return TrackColumn::AlbumArtist;
      case rt::TrackPresentationField::Genre: return TrackColumn::Genre;
      case rt::TrackPresentationField::Composer: return TrackColumn::Composer;
      case rt::TrackPresentationField::Work: return TrackColumn::Work;
      case rt::TrackPresentationField::Year: return TrackColumn::Year;
      case rt::TrackPresentationField::DiscNumber: return TrackColumn::DiscNumber;
      case rt::TrackPresentationField::TrackNumber: return TrackColumn::TrackNumber;
      case rt::TrackPresentationField::Duration: return TrackColumn::Duration;
      case rt::TrackPresentationField::Tags: return TrackColumn::Tags;
    }

    return std::nullopt;
  }

  std::span<TrackColumnDefinition const> trackColumnDefinitions()
  {
    return kTrackColumnDefinitions;
  }

  TrackColumnDefinition const* trackColumnDefinition(TrackColumn column)
  {
    auto const* const it = std::ranges::find(kTrackColumnDefinitions, column, &TrackColumnDefinition::column);

    if (it == kTrackColumnDefinitions.end())
    {
      return nullptr;
    }

    return &*it;
  }

  TrackColumnState defaultTrackColumnState(TrackColumn column)
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

  TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackPresentationSpec const& presentation)
  {
    auto layout = TrackColumnLayout{};

    for (auto const field : presentation.visibleFields)
    {
      auto const optColumn = trackColumnForPresentationField(field);

      if (!optColumn)
      {
        continue;
      }

      auto state = defaultTrackColumnState(*optColumn);
      state.visible = true;
      layout.columns.push_back(state);
    }

    return normalizeTrackColumnLayout(layout);
  }

  TrackColumnLayout trackColumnLayoutForPresentation(rt::TrackListPresentationSnapshot const& presentation)
  {
    auto spec = rt::TrackPresentationSpec{
      .id = presentation.presentationId,
      .groupBy = presentation.groupBy,
      .sortBy = presentation.effectiveSortBy,
      .visibleFields = presentation.visibleFields,
      .redundantFields = presentation.redundantFields,
    };

    return trackColumnLayoutForPresentation(spec);
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
        auto state = defaultColumnState(definition.column);
        state.visible = false; // Missing from explicit layout means it should be hidden
        result.columns.push_back(state);
      }
    }

    return result;
  }

  std::vector<TrackColumn> expandingTrackColumnsForLayout(TrackColumnLayout const& layout)
  {
    auto const normalized = normalizeTrackColumnLayout(layout);
    auto columns = std::vector<TrackColumn>{};

    for (auto const& state : normalized.columns)
    {
      if (!state.visible)
      {
        continue;
      }

      auto const* const definition = trackColumnDefinition(state.column);

      if (definition != nullptr && definition->expands)
      {
        columns.push_back(state.column);
      }
    }

    if (!columns.empty())
    {
      return columns;
    }

    auto const title = std::ranges::find(normalized.columns, TrackColumn::Title, &TrackColumnState::column);

    if (title != normalized.columns.end() && title->visible)
    {
      columns.push_back(TrackColumn::Title);
      return columns;
    }

    auto const firstVisible = std::ranges::find(normalized.columns, true, &TrackColumnState::visible);

    if (firstVisible != normalized.columns.end())
    {
      columns.push_back(firstVisible->column);
    }

    return columns;
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
