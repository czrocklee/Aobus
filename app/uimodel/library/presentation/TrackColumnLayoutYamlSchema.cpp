// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>
#include <ao/yaml/Serialization.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    Result<> writeColumn(ryml::NodeRef node, StoredTrackColumn const& column)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("field", column.field).scalar("width", column.width).scalar("weight", column.weight);
      return {};
    }

    Result<StoredTrackColumn> readColumn(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"field", "width", "weight"});

      auto column = StoredTrackColumn{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("field", column.field)
        .requiredScalar("width", column.width)
        .requiredScalar("weight", column.weight);
      return std::move(reader).finish(std::move(column));
    }

    Result<> writeLayout(ryml::NodeRef node, StoredTrackColumnLayout const& layout)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("listId", layout.listId).sequence("columns", layout.columns, writeColumn);
      return std::move(writer).finish();
    }

    Result<StoredTrackColumnLayout> readLayout(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"listId", "columns"});

      auto layout = StoredTrackColumnLayout{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("listId", layout.listId).requiredSequence("columns", layout.columns, readColumn);
      return std::move(reader).finish(std::move(layout));
    }

    Result<> writeDocument(ryml::NodeRef node, TrackColumnLayoutDocument const& document)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("version", document.version).sequence("layouts", document.layouts, writeLayout);
      return std::move(writer).finish();
    }

    Result<TrackColumnLayoutDocument> readDocument(ryml::ConstNodeRef node)
    {
      constexpr auto kContext = std::string_view{"track column layout"};

      if (auto const result = yaml::requireMap(node, kContext); !result)
      {
        return std::unexpected{result.error()};
      }

      auto version = yaml::requireScalar<std::uint32_t>(node, "version", kContext);

      if (!version)
      {
        return std::unexpected{version.error()};
      }

      if (*version != kTrackColumnLayoutVersion)
      {
        return makeError(
          Error::Code::NotSupported, std::format("Unsupported track column layout version {}", *version));
      }

      constexpr auto kKeys = std::to_array<std::string_view>({"version", "layouts"});

      auto document = TrackColumnLayoutDocument{.version = *version};
      auto reader = yaml::MapReader{node, kKeys, kContext};
      reader.requiredSequence("layouts", document.layouts, readLayout);
      return std::move(reader).finish(std::move(document));
    }

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

  Result<TrackColumnLayoutDocument> toTrackColumnLayoutDocument(TrackColumnLayoutState const& state)
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

  Result<TrackColumnLayoutState> trackColumnLayoutStateFromDocument(TrackColumnLayoutDocument const& document)
  {
    if (document.version != kTrackColumnLayoutVersion)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Unsupported track column layout version {}", document.version));
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

        auto columnState = TrackColumnState{
          .field = *optField,
          .width = column.width,
          .weight = column.weight,
        };

        if (auto const valid = validateColumn(columnState, Error::Code::FormatRejected); !valid)
        {
          return std::unexpected{valid.error()};
        }

        columns.push_back(columnState);
      }

      state.listLayouts.emplace(listId, std::move(columns));
    }

    return state;
  }

  Result<> TrackColumnLayoutYamlSchema::serialize(ryml::NodeRef node, TrackColumnLayoutState const& state) const
  {
    auto document = toTrackColumnLayoutDocument(state);

    if (!document)
    {
      return std::unexpected{document.error()};
    }

    return writeDocument(node, *document);
  }

  Result<TrackColumnLayoutState> TrackColumnLayoutYamlSchema::deserialize(ryml::ConstNodeRef node,
                                                                          TrackColumnLayoutState const& /*seed*/) const
  {
    auto document = readDocument(node);

    if (!document)
    {
      return std::unexpected{document.error()};
    }

    return trackColumnLayoutStateFromDocument(*document);
  }
} // namespace ao::uimodel
