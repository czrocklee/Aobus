// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WorkspaceSessionYamlSchema.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/utility/StrongTypeFormatter.h>
#include <ao/yaml/Serialization.h>

#include <gsl-lite/gsl-lite.hpp>

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
    Result<std::string> storedIdFor(Enum value, IdFunction idFunction, [[maybe_unused]] std::string_view context)
    {
      auto const id = idFunction(value);

      gsl_Expects(!id.empty() && "Cannot serialize invalid id");
      return std::string{id};
    }

    Result<std::vector<std::string>> toStoredFields(std::vector<TrackField> const& fields,
                                                    [[maybe_unused]] std::string_view context)
    {
      auto stored = std::vector<std::string>{};
      stored.reserve(fields.size());

      for (auto const field : fields)
      {
        auto storedIdRes = storedIdFor(field, trackFieldId, context);

        if (!storedIdRes)
        {
          return std::unexpected{storedIdRes.error()};
        }

        gsl_Expects(!std::ranges::contains(stored, *storedIdRes) && "Cannot serialize duplicate field");
        stored.push_back(std::move(*storedIdRes));
      }

      return stored;
    }

    Result<StoredTrackPresentationSpec> toStoredPresentation(TrackPresentationSpec const& spec)
    {
      gsl_Expects(!spec.id.empty() && "Cannot serialize a presentation with an empty id");
      auto const normalized = normalizeTrackPresentationSpec(spec);
      auto groupRes = storedIdFor(normalized.groupBy, trackGroupKeyId, "track group key");

      if (!groupRes)
      {
        return std::unexpected{groupRes.error()};
      }

      auto stored = StoredTrackPresentationSpec{.id = normalized.id, .group = std::move(*groupRes)};
      stored.sort.reserve(normalized.sortBy.size());

      for (auto const& term : normalized.sortBy)
      {
        auto fieldRes = storedIdFor(term.field, trackSortFieldId, "track sort field");

        if (!fieldRes)
        {
          return std::unexpected{fieldRes.error()};
        }

        gsl_Expects(!std::ranges::contains(stored.sort, *fieldRes, &StoredTrackSortTerm::field) &&
                    "Cannot serialize duplicate sort field");
        stored.sort.push_back(StoredTrackSortTerm{
          .field = std::move(*fieldRes),
          .direction = std::string{term.ascending ? kAscending : kDescending},
        });
      }

      auto visibleFieldsRes = toStoredFields(normalized.visibleFields, "visible field");

      if (!visibleFieldsRes)
      {
        return std::unexpected{visibleFieldsRes.error()};
      }

      auto redundantFieldsRes = toStoredFields(normalized.redundantFields, "redundant field");

      if (!redundantFieldsRes)
      {
        return std::unexpected{redundantFieldsRes.error()};
      }

      stored.visibleFields = std::move(*visibleFieldsRes);
      stored.redundantFields = std::move(*redundantFieldsRes);
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

      auto visibleFieldsRes = trackFieldsFromStored(stored.visibleFields, "visible field");

      if (!visibleFieldsRes)
      {
        return std::unexpected{visibleFieldsRes.error()};
      }

      if (visibleFieldsRes->empty())
      {
        return makeError(Error::Code::FormatRejected, "Track presentation has no visible fields");
      }

      auto redundantFieldsRes = trackFieldsFromStored(stored.redundantFields, "redundant field");

      if (!redundantFieldsRes)
      {
        return std::unexpected{redundantFieldsRes.error()};
      }

      spec.visibleFields = std::move(*visibleFieldsRes);
      spec.redundantFields = std::move(*redundantFieldsRes);
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

      auto presentationVersionRes = yaml::requireScalar<std::uint32_t>(node, "presentationVersion", kContext);

      if (!presentationVersionRes)
      {
        return std::unexpected{presentationVersionRes.error()};
      }

      if (*presentationVersionRes != kWorkspacePresentationVersion)
      {
        return makeError(Error::Code::NotSupported,
                         std::format("Unsupported workspace presentation version {}", *presentationVersionRes));
      }

      constexpr auto kKeys =
        std::to_array<std::string_view>({"presentationVersion", "openViews", "activeViewIndex", "customPresets"});

      auto document = WorkspaceSessionDocument{.presentationVersion = *presentationVersionRes};
      auto reader = yaml::MapReader{node, kKeys, kContext};
      reader.requiredSequence("openViews", document.openViews, readView)
        .requiredScalar("activeViewIndex", document.activeViewIndex)
        .requiredSequence("customPresets", document.customPresets, readPreset);
      return std::move(reader).finish(std::move(document));
    }
  } // namespace

  Result<WorkspaceSessionDocument> toWorkspaceSessionDocument(WorkspaceSessionState const& state)
  {
    gsl_Expects(state.activeViewIndex <= std::numeric_limits<std::uint32_t>::max() &&
                "Workspace active view index is not representable");

    gsl_Expects((!state.openViews.empty() || state.activeViewIndex == 0) &&
                (state.openViews.empty() || state.activeViewIndex < state.openViews.size()) &&
                "Workspace active view index is out of bounds");

    auto document = WorkspaceSessionDocument{
      .presentationVersion = kWorkspacePresentationVersion,
      .activeViewIndex = static_cast<std::uint32_t>(state.activeViewIndex),
    };
    document.openViews.reserve(state.openViews.size());

    for (auto const& view : state.openViews)
    {
      gsl_Expects(view.listId != kInvalidListId && "Workspace view uses the invalid list id");
      gsl_Expects(view.optPresentation && "Workspace view has no exact presentation to persist");

      auto presentationRes = toStoredPresentation(*view.optPresentation);

      if (!presentationRes)
      {
        return std::unexpected{presentationRes.error()};
      }

      document.openViews.push_back(StoredTrackListViewConfig{
        .listId = view.listId.raw(),
        .filterExpression = view.filterExpression,
        .presentation = std::move(*presentationRes),
      });
    }

    document.customPresets.reserve(state.customPresets.size());

    for (auto const& preset : state.customPresets)
    {
      auto specRes = toStoredPresentation(preset.spec);

      if (!specRes)
      {
        return std::unexpected{specRes.error()};
      }

      document.customPresets.push_back(StoredCustomTrackPresentationPreset{
        .label = preset.label,
        .basePresetId = preset.basePresetId,
        .spec = std::move(*specRes),
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

      auto presentationRes = trackPresentationFromStored(stored.presentation);

      if (!presentationRes)
      {
        return std::unexpected{presentationRes.error()};
      }

      state.openViews.push_back(TrackListViewConfig{
        .listId = ListId{stored.listId},
        .filterExpression = stored.filterExpression,
        .groupBy = presentationRes->groupBy,
        .sortBy = presentationRes->sortBy,
        .optPresentation = std::move(*presentationRes),
      });
    }

    state.customPresets.reserve(document.customPresets.size());

    for (auto const& stored : document.customPresets)
    {
      auto specRes = trackPresentationFromStored(stored.spec);

      if (!specRes)
      {
        return std::unexpected{specRes.error()};
      }

      state.customPresets.push_back(CustomTrackPresentationPreset{
        .label = stored.label,
        .basePresetId = stored.basePresetId,
        .spec = std::move(*specRes),
      });
    }

    return state;
  }

  Result<> WorkspaceSessionYamlSchema::serialize(ryml::NodeRef node, WorkspaceSessionState const& state) const
  {
    auto documentRes = toWorkspaceSessionDocument(state);

    if (!documentRes)
    {
      return std::unexpected{documentRes.error()};
    }

    return writeDocument(node, *documentRes);
  }

  Result<WorkspaceSessionState> WorkspaceSessionYamlSchema::deserialize(ryml::ConstNodeRef node,
                                                                        WorkspaceSessionState const& /*seed*/) const
  {
    auto documentRes = readDocument(node);

    if (!documentRes)
    {
      return std::unexpected{documentRes.error()};
    }

    return workspaceSessionStateFromDocument(*documentRes);
  }
} // namespace ao::rt::detail
