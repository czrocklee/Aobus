// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WorkspaceSessionYamlSchema.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/yaml/Serialization.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::detail
{
  namespace
  {
    constexpr std::string_view kAscending = "ascending";
    constexpr std::string_view kDescending = "descending";

    Result<> validateActiveViewIndex(std::size_t const activeViewIndex,
                                     std::size_t const openViewCount,
                                     Error::Code const errorCode)
    {
      if ((openViewCount == 0 && activeViewIndex != 0) || (openViewCount != 0 && activeViewIndex >= openViewCount))
      {
        return makeError(errorCode, "Workspace active view index is out of bounds");
      }

      return {};
    }

    template<typename Enum, typename IdFunction>
    Result<std::string> storedIdFor(Enum value, IdFunction idFunction, std::string_view context)
    {
      auto const id = idFunction(value);

      if (id.empty())
      {
        return makeError(Error::Code::InvalidState, std::format("Cannot serialize invalid {}", context));
      }

      return std::string{id};
    }

    Result<std::vector<std::string>> toStoredFields(std::vector<TrackField> const& fields, std::string_view context)
    {
      auto stored = std::vector<std::string>{};
      stored.reserve(fields.size());

      for (auto const field : fields)
      {
        auto storedId = storedIdFor(field, trackFieldId, context);

        if (!storedId)
        {
          return std::unexpected{storedId.error()};
        }

        if (std::ranges::contains(stored, *storedId))
        {
          return makeError(
            Error::Code::InvalidState, std::format("Cannot serialize duplicate {} '{}'", context, *storedId));
        }

        stored.push_back(std::move(*storedId));
      }

      return stored;
    }

    Result<StoredTrackPresentationSpec> toStoredPresentation(TrackPresentationSpec const& spec)
    {
      if (spec.id.empty())
      {
        return makeError(Error::Code::InvalidState, "Cannot serialize a presentation with an empty id");
      }

      auto const normalized = normalizeTrackPresentationSpec(spec);
      auto group = storedIdFor(normalized.groupBy, trackGroupKeyId, "track group key");

      if (!group)
      {
        return std::unexpected{group.error()};
      }

      auto stored = StoredTrackPresentationSpec{.id = normalized.id, .group = std::move(*group)};
      stored.sort.reserve(normalized.sortBy.size());

      for (auto const& term : normalized.sortBy)
      {
        auto field = storedIdFor(term.field, trackSortFieldId, "track sort field");

        if (!field)
        {
          return std::unexpected{field.error()};
        }

        if (std::ranges::contains(stored.sort, *field, &StoredTrackSortTerm::field))
        {
          return makeError(
            Error::Code::InvalidState, std::format("Cannot serialize duplicate sort field '{}'", *field));
        }

        stored.sort.push_back(StoredTrackSortTerm{
          .field = std::move(*field),
          .direction = std::string{term.ascending ? kAscending : kDescending},
        });
      }

      auto visibleFields = toStoredFields(normalized.visibleFields, "visible field");

      if (!visibleFields)
      {
        return std::unexpected{visibleFields.error()};
      }

      auto redundantFields = toStoredFields(normalized.redundantFields, "redundant field");

      if (!redundantFields)
      {
        return std::unexpected{redundantFields.error()};
      }

      stored.visibleFields = std::move(*visibleFields);
      stored.redundantFields = std::move(*redundantFields);
      return stored;
    }

    Result<std::vector<TrackField>> trackFieldsFromStored(std::vector<std::string> const& stored,
                                                          std::string_view context)
    {
      auto fields = std::vector<TrackField>{};
      fields.reserve(stored.size());

      for (auto const& id : stored)
      {
        auto const optField = trackFieldFromId(id);

        if (!optField)
        {
          return makeError(Error::Code::FormatRejected, std::format("Unknown {} '{}'", context, id));
        }

        if (std::ranges::contains(fields, *optField))
        {
          return makeError(Error::Code::FormatRejected, std::format("Duplicate {} '{}'", context, id));
        }

        fields.push_back(*optField);
      }

      return fields;
    }

    Result<TrackPresentationSpec> trackPresentationFromStored(StoredTrackPresentationSpec const& stored)
    {
      if (stored.id.empty())
      {
        return makeError(Error::Code::FormatRejected, "Track presentation uses an empty id");
      }

      auto const optGroup = trackGroupKeyFromId(stored.group);

      if (!optGroup)
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown track group key '{}'", stored.group));
      }

      auto spec = TrackPresentationSpec{.id = stored.id, .groupBy = *optGroup};
      spec.sortBy.reserve(stored.sort.size());

      for (auto const& term : stored.sort)
      {
        auto const optField = trackSortFieldFromId(term.field);

        if (!optField)
        {
          return makeError(Error::Code::FormatRejected, std::format("Unknown track sort field '{}'", term.field));
        }

        if (std::ranges::contains(spec.sortBy, *optField, &TrackSortTerm::field))
        {
          return makeError(Error::Code::FormatRejected, std::format("Duplicate track sort field '{}'", term.field));
        }

        if (term.direction != kAscending && term.direction != kDescending)
        {
          return makeError(
            Error::Code::FormatRejected, std::format("Unknown track sort direction '{}'", term.direction));
        }

        spec.sortBy.push_back(TrackSortTerm{.field = *optField, .ascending = term.direction == kAscending});
      }

      auto visibleFields = trackFieldsFromStored(stored.visibleFields, "visible field");

      if (!visibleFields)
      {
        return std::unexpected{visibleFields.error()};
      }

      if (visibleFields->empty())
      {
        return makeError(Error::Code::FormatRejected, "Track presentation has no visible fields");
      }

      auto redundantFields = trackFieldsFromStored(stored.redundantFields, "redundant field");

      if (!redundantFields)
      {
        return std::unexpected{redundantFields.error()};
      }

      spec.visibleFields = std::move(*visibleFields);
      spec.redundantFields = std::move(*redundantFields);
      return spec;
    }

    Result<> writeSortTerm(ryml::NodeRef node, StoredTrackSortTerm const& term)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("field", term.field).scalar("direction", term.direction);
      return {};
    }

    Result<StoredTrackSortTerm> readSortTerm(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"field", "direction"});
      auto term = StoredTrackSortTerm{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("field", term.field).requiredScalar("direction", term.direction);
      return std::move(reader).finish(std::move(term));
    }

    Result<> writePresentation(ryml::NodeRef node, StoredTrackPresentationSpec const& presentation)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("id", presentation.id)
        .scalar("group", presentation.group)
        .sequence("sort", presentation.sort, writeSortTerm)
        .scalarSequence("visibleFields", presentation.visibleFields)
        .scalarSequence("redundantFields", presentation.redundantFields);
      return std::move(writer).finish();
    }

    Result<StoredTrackPresentationSpec> readPresentation(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys =
        std::to_array<std::string_view>({"id", "group", "sort", "visibleFields", "redundantFields"});
      auto presentation = StoredTrackPresentationSpec{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("id", presentation.id)
        .requiredScalar("group", presentation.group)
        .requiredSequence("sort", presentation.sort, readSortTerm)
        .requiredScalarSequence("visibleFields", presentation.visibleFields)
        .requiredScalarSequence("redundantFields", presentation.redundantFields);
      return std::move(reader).finish(std::move(presentation));
    }

    Result<> writeView(ryml::NodeRef node, StoredTrackListViewConfig const& view)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("listId", view.listId)
        .scalar("filterExpression", view.filterExpression)
        .value("presentation", view.presentation, writePresentation);
      return std::move(writer).finish();
    }

    Result<StoredTrackListViewConfig> readView(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"listId", "filterExpression", "presentation"});
      auto view = StoredTrackListViewConfig{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("listId", view.listId)
        .requiredScalar("filterExpression", view.filterExpression)
        .requiredValue("presentation", view.presentation, readPresentation);
      return std::move(reader).finish(std::move(view));
    }

    Result<> writePreset(ryml::NodeRef node, StoredCustomTrackPresentationPreset const& preset)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("label", preset.label)
        .scalar("basePresetId", preset.basePresetId)
        .value("spec", preset.spec, writePresentation);
      return std::move(writer).finish();
    }

    Result<StoredCustomTrackPresentationPreset> readPreset(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"label", "basePresetId", "spec"});
      auto preset = StoredCustomTrackPresentationPreset{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("label", preset.label)
        .requiredScalar("basePresetId", preset.basePresetId)
        .requiredValue("spec", preset.spec, readPresentation);
      return std::move(reader).finish(std::move(preset));
    }

    Result<> writeDocument(ryml::NodeRef node, WorkspaceSessionDocument const& document)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("presentationVersion", document.presentationVersion)
        .sequence("openViews", document.openViews, writeView)
        .scalar("activeViewIndex", document.activeViewIndex)
        .sequence("customPresets", document.customPresets, writePreset);
      return std::move(writer).finish();
    }

    Result<WorkspaceSessionDocument> readDocument(ryml::ConstNodeRef node)
    {
      constexpr auto kContext = std::string_view{"workspace"};

      if (auto const result = yaml::requireMap(node, kContext); !result)
      {
        return std::unexpected{result.error()};
      }

      auto presentationVersion = yaml::requireScalar<std::uint32_t>(node, "presentationVersion", kContext);

      if (!presentationVersion)
      {
        return std::unexpected{presentationVersion.error()};
      }

      if (*presentationVersion != kWorkspacePresentationVersion)
      {
        return makeError(Error::Code::NotSupported,
                         std::format("Unsupported workspace presentation version {}", *presentationVersion));
      }

      constexpr auto kKeys =
        std::to_array<std::string_view>({"presentationVersion", "openViews", "activeViewIndex", "customPresets"});

      auto document = WorkspaceSessionDocument{.presentationVersion = *presentationVersion};
      auto reader = yaml::MapReader{node, kKeys, kContext};
      reader.requiredSequence("openViews", document.openViews, readView)
        .requiredScalar("activeViewIndex", document.activeViewIndex)
        .requiredSequence("customPresets", document.customPresets, readPreset);
      return std::move(reader).finish(std::move(document));
    }
  } // namespace

  Result<WorkspaceSessionDocument> toWorkspaceSessionDocument(WorkspaceSessionState const& state)
  {
    if (state.activeViewIndex > std::numeric_limits<std::uint32_t>::max())
    {
      return makeError(Error::Code::InvalidState, "Workspace active view index is not representable");
    }

    if (auto const result =
          validateActiveViewIndex(state.activeViewIndex, state.openViews.size(), Error::Code::InvalidState);
        !result)
    {
      return std::unexpected{result.error()};
    }

    auto document = WorkspaceSessionDocument{
      .presentationVersion = kWorkspacePresentationVersion,
      .activeViewIndex = static_cast<std::uint32_t>(state.activeViewIndex),
    };
    document.openViews.reserve(state.openViews.size());

    for (auto const& view : state.openViews)
    {
      if (view.listId == kInvalidListId)
      {
        return makeError(Error::Code::InvalidState, "Workspace view uses the invalid list id");
      }

      if (!view.optPresentation)
      {
        return makeError(Error::Code::InvalidState, "Workspace view has no exact presentation to persist");
      }

      auto presentation = toStoredPresentation(*view.optPresentation);

      if (!presentation)
      {
        return std::unexpected{presentation.error()};
      }

      document.openViews.push_back(StoredTrackListViewConfig{
        .listId = view.listId.raw(),
        .filterExpression = view.filterExpression,
        .presentation = std::move(*presentation),
      });
    }

    document.customPresets.reserve(state.customPresets.size());

    for (auto const& preset : state.customPresets)
    {
      auto spec = toStoredPresentation(preset.spec);

      if (!spec)
      {
        return std::unexpected{spec.error()};
      }

      document.customPresets.push_back(StoredCustomTrackPresentationPreset{
        .label = preset.label,
        .basePresetId = preset.basePresetId,
        .spec = std::move(*spec),
      });
    }

    return document;
  }

  Result<WorkspaceSessionState> workspaceSessionStateFromDocument(WorkspaceSessionDocument const& document)
  {
    if (document.presentationVersion != kWorkspacePresentationVersion)
    {
      return makeError(Error::Code::NotSupported,
                       std::format("Unsupported workspace presentation version {}", document.presentationVersion));
    }

    if (auto const result =
          validateActiveViewIndex(document.activeViewIndex, document.openViews.size(), Error::Code::FormatRejected);
        !result)
    {
      return std::unexpected{result.error()};
    }

    auto state = WorkspaceSessionState{.activeViewIndex = document.activeViewIndex};
    state.openViews.reserve(document.openViews.size());

    for (auto const& stored : document.openViews)
    {
      if (stored.listId == kInvalidListId.raw())
      {
        return makeError(Error::Code::FormatRejected, "Workspace view uses the invalid list id");
      }

      auto presentation = trackPresentationFromStored(stored.presentation);

      if (!presentation)
      {
        return std::unexpected{presentation.error()};
      }

      state.openViews.push_back(TrackListViewConfig{
        .listId = ListId{stored.listId},
        .filterExpression = stored.filterExpression,
        .groupBy = presentation->groupBy,
        .sortBy = presentation->sortBy,
        .optPresentation = std::move(*presentation),
      });
    }

    state.customPresets.reserve(document.customPresets.size());

    for (auto const& stored : document.customPresets)
    {
      auto spec = trackPresentationFromStored(stored.spec);

      if (!spec)
      {
        return std::unexpected{spec.error()};
      }

      state.customPresets.push_back(CustomTrackPresentationPreset{
        .label = stored.label,
        .basePresetId = stored.basePresetId,
        .spec = std::move(*spec),
      });
    }

    return state;
  }

  Result<> WorkspaceSessionYamlSchema::serialize(ryml::NodeRef node, WorkspaceSessionState const& state) const
  {
    auto document = toWorkspaceSessionDocument(state);

    if (!document)
    {
      return std::unexpected{document.error()};
    }

    return writeDocument(node, *document);
  }

  Result<WorkspaceSessionState> WorkspaceSessionYamlSchema::deserialize(ryml::ConstNodeRef node,
                                                                        WorkspaceSessionState const& /*seed*/) const
  {
    auto document = readDocument(node);

    if (!document)
    {
      return std::unexpected{document.error()};
    }

    return workspaceSessionStateFromDocument(*document);
  }
} // namespace ao::rt::detail
