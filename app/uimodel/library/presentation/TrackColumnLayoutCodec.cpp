// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutCodec.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <algorithm>
#include <cmath>
#include <expected>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    bool matchesColumnSizingPolicy(TrackColumnState const& column)
    {
      auto const fixed = column.width > 0 && column.weight == -1.0;
      auto const flexible = column.width == -1 && column.weight > 0.0 && std::isfinite(column.weight);
      return trackFieldColumnSizing(column.field) == TrackColumnSizing::Fixed ? fixed : flexible;
    }

    Result<> validateColumn(TrackColumnState const& column, Error::Code code)
    {
      if (rt::trackFieldId(column.field).empty())
      {
        return makeError(code, "Track column contains an unknown field");
      }

      if (!matchesColumnSizingPolicy(column))
      {
        return makeError(code, "Track column width and weight do not match the field sizing policy");
      }

      return {};
    }
  } // namespace

  Result<TrackColumnLayoutDocument> encodeTrackColumnLayout(TrackColumnLayoutState const& state)
  {
    auto document = TrackColumnLayoutDocument{};
    document.layouts.reserve(state.listLayouts.size());

    for (auto const& [listId, columns] : state.listLayouts)
    {
      if (listId == kInvalidListId)
      {
        return makeError(Error::Code::InvalidState, "Cannot persist a track column layout for the invalid list id");
      }

      auto stored = StoredTrackColumnLayout{.listId = listId.raw()};
      stored.columns.reserve(columns.size());

      for (auto const& column : columns)
      {
        if (auto const valid = validateColumn(column, Error::Code::InvalidState); !valid)
        {
          return std::unexpected{valid.error()};
        }

        auto const field = rt::trackFieldId(column.field);

        if (std::ranges::contains(stored.columns, field, &StoredTrackColumn::field))
        {
          return makeError(Error::Code::InvalidState, std::format("Cannot persist duplicate track column '{}'", field));
        }

        stored.columns.push_back(StoredTrackColumn{
          .field = std::string{field},
          .width = column.width,
          .weight = column.weight,
        });
      }

      document.layouts.push_back(std::move(stored));
    }

    return document;
  }

  Result<TrackColumnLayoutState> decodeTrackColumnLayout(TrackColumnLayoutDocument const& document)
  {
    if (document.version != kTrackColumnLayoutVersion)
    {
      return makeError(
        Error::Code::FormatRejected, std::format("Unsupported track column layout version {}", document.version));
    }

    auto state = TrackColumnLayoutState{};

    for (auto const& stored : document.layouts)
    {
      auto const listId = ListId{stored.listId};

      if (listId == kInvalidListId)
      {
        return makeError(Error::Code::FormatRejected, "Track column layout uses the invalid list id");
      }

      if (state.listLayouts.contains(listId))
      {
        return makeError(
          Error::Code::FormatRejected, std::format("Track column layout repeats list id {}", stored.listId));
      }

      auto columns = std::vector<TrackColumnState>{};
      columns.reserve(stored.columns.size());

      for (auto const& column : stored.columns)
      {
        auto const optField = rt::trackFieldFromId(column.field);

        if (!optField)
        {
          return makeError(Error::Code::FormatRejected, std::format("Unknown track column field '{}'", column.field));
        }

        if (std::ranges::contains(columns, *optField, &TrackColumnState::field))
        {
          return makeError(Error::Code::FormatRejected, std::format("Duplicate track column field '{}'", column.field));
        }

        auto decoded = TrackColumnState{
          .field = *optField,
          .width = column.width,
          .weight = column.weight,
        };

        if (auto const valid = validateColumn(decoded, Error::Code::FormatRejected); !valid)
        {
          return std::unexpected{valid.error()};
        }

        columns.push_back(decoded);
      }

      state.listLayouts.emplace(listId, std::move(columns));
    }

    return state;
  }
} // namespace ao::uimodel
