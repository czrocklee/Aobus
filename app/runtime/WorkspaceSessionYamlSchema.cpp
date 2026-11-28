// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WorkspaceSessionYamlSchema.h"

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/utility/StrongTypeFormatter.h>
#include <ao/utility/Uuid.h>
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
#include <variant>
#include <vector>

namespace ao::rt::detail
{
  namespace
  {
    constexpr std::string_view kBuiltin = "builtin";
    constexpr std::string_view kCustom = "custom";
    constexpr std::string_view kAscending = "ascending";
    constexpr std::string_view kDescending = "descending";

    bool isValidActiveViewIndex(std::size_t const index, std::size_t const viewCount) noexcept
    {
      return viewCount == 0 ? index == 0 : index < viewCount;
    }

    std::vector<std::string> toStoredFields(std::vector<TrackField> const& fields)
    {
      auto stored = std::vector<std::string>{};
      stored.reserve(fields.size());

      for (auto const field : fields)
      {
        auto const id = trackFieldId(field);
        AO_EXPECTS(!id.empty(), "Cannot serialize invalid id");
        AO_EXPECTS(!std::ranges::contains(stored, id), "Cannot serialize duplicate field");
        stored.emplace_back(id);
      }

      return stored;
    }

    StoredTrackSortTerm toStoredSortTerm(TrackSortTerm const& term)
    {
      auto const field = trackSortFieldId(term.field);
      AO_EXPECTS(!field.empty(), "Cannot serialize an invalid sort field");
      return StoredTrackSortTerm{
        .field = std::string{field}, .direction = std::string{term.ascending ? kAscending : kDescending}};
    }

    StoredTrackPresentationSpec toStoredPresentation(TrackPresentationSpec const& spec)
    {
      auto const normalized = normalizeTrackPresentationSpec(spec);
      auto const group = trackGroupKeyId(normalized.groupBy);
      AO_EXPECTS(!group.empty(), "Cannot serialize invalid id");
      auto stored = StoredTrackPresentationSpec{.presetId = normalized.presetId, .group = std::string{group}};
      stored.sort.reserve(normalized.sortBy.size());

      for (auto const& term : normalized.sortBy)
      {
        auto storedTerm = toStoredSortTerm(term);
        AO_EXPECTS(!std::ranges::contains(stored.sort, storedTerm.field, &StoredTrackSortTerm::field),
                   "Cannot serialize duplicate sort field");
        stored.sort.push_back(std::move(storedTerm));
      }

      if (normalized.optColumnSort)
      {
        auto const& sort = *normalized.optColumnSort;
        AO_EXPECTS(trackSortFieldForColumn(sort.column), "Cannot serialize an unsortable column");
        stored.optColumnSort =
          StoredTrackColumnSort{.column = std::string{trackFieldId(sort.column)},
                                .direction = std::string{sort.ascending ? kAscending : kDescending}};
      }

      stored.visibleFields = toStoredFields(normalized.visibleFields);
      stored.redundantFields = toStoredFields(normalized.redundantFields);
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

    Result<bool> ascendingFromStored(std::string_view direction)
    {
      if (direction != kAscending && direction != kDescending)
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown track sort direction '{}'", direction));
      }

      return direction == kAscending;
    }

    Result<TrackSortTerm> sortTermFromStored(StoredTrackSortTerm const& stored)
    {
      auto const optField = trackSortFieldFromId(stored.field);

      if (!optField)
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown track sort field '{}'", stored.field));
      }

      auto const ascendingRes = ascendingFromStored(stored.direction);

      if (!ascendingRes)
      {
        return std::unexpected{ascendingRes.error()};
      }

      return TrackSortTerm{.field = *optField, .ascending = *ascendingRes};
    }

    Result<TrackPresentationSpec> trackPresentationFromStored(StoredTrackPresentationSpec const& stored)
    {
      auto const optGroup = trackGroupKeyFromId(stored.group);

      if (!optGroup)
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown track group key '{}'", stored.group));
      }

      auto spec = TrackPresentationSpec{.presetId = stored.presetId, .groupBy = *optGroup};
      spec.sortBy.reserve(stored.sort.size());

      for (auto const& term : stored.sort)
      {
        auto const termRes = sortTermFromStored(term);

        if (!termRes)
        {
          return std::unexpected{termRes.error()};
        }

        if (std::ranges::contains(spec.sortBy, termRes->field, &TrackSortTerm::field))
        {
          return makeError(Error::Code::FormatRejected, std::format("Duplicate track sort field '{}'", term.field));
        }

        spec.sortBy.push_back(*termRes);
      }

      if (stored.optColumnSort)
      {
        auto const& sort = *stored.optColumnSort;
        auto const optColumn = trackFieldFromId(sort.column);

        if (!optColumn || !trackSortFieldForColumn(*optColumn))
        {
          return makeError(Error::Code::FormatRejected, std::format("Unknown or unsortable column '{}'", sort.column));
        }

        auto const ascendingRes = ascendingFromStored(sort.direction);

        if (!ascendingRes)
        {
          return std::unexpected{ascendingRes.error()};
        }

        spec.optColumnSort = TrackColumnSort{.column = *optColumn, .ascending = *ascendingRes};
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

    Result<> writeColumnSort(ryml::NodeRef node, StoredTrackColumnSort const& sort)
    {
      auto writer = yaml::MapWriter{node};
      writer.scalar("column", sort.column).scalar("direction", sort.direction);
      return std::move(writer).finish();
    }

    Result<StoredTrackColumnSort> readColumnSort(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>({"column", "direction"});
      auto sort = StoredTrackColumnSort{};
      auto reader = yaml::MapReader{node, kKeys, context};
      reader.requiredScalar("column", sort.column).requiredScalar("direction", sort.direction);
      return std::move(reader).finish(std::move(sort));
    }

    Result<> writePresentation(ryml::NodeRef node, StoredTrackPresentationSpec const& presentation)
    {
      auto writer = yaml::MapWriter{node};

      auto const kind = std::holds_alternative<BuiltinPreset>(presentation.presetId) ? kBuiltin : kCustom;
      writer.scalar("kind", kind)
        .scalar("presetId", trackPresetKey(presentation.presetId))
        .scalar("group", presentation.group)
        .sequence("sort", presentation.sort, writeSortTerm);

      if (presentation.optColumnSort)
      {
        writer.value("columnSort", *presentation.optColumnSort, writeColumnSort);
      }

      writer.scalarSequence("visibleFields", presentation.visibleFields)
        .scalarSequence("redundantFields", presentation.redundantFields);
      return std::move(writer).finish();
    }

    Result<StoredTrackPresentationSpec> readPresentation(ryml::ConstNodeRef node, std::string_view context)
    {
      constexpr auto kKeys = std::to_array<std::string_view>(
        {"kind", "presetId", "group", "sort", "columnSort", "visibleFields", "redundantFields"});
      auto presentation = StoredTrackPresentationSpec{};
      auto reader = yaml::MapReader{node, kKeys, context};
      auto kind = std::string{};
      auto key = std::string{};

      if (auto const& res = reader.requiredScalar("kind", kind).requiredScalar("presetId", key).result(); !res)
      {
        return std::unexpected{res.error()};
      }

      if (kind != kBuiltin && kind != kCustom)
      {
        return makeError(Error::Code::FormatRejected, std::format("Unknown presentation kind '{}'", kind));
      }

      auto const optId = parseTrackPresetId(key);

      if (!optId || (kind == kBuiltin) != std::holds_alternative<BuiltinPreset>(*optId))
      {
        return makeError(Error::Code::FormatRejected, std::format("Invalid {} preset id '{}'", kind, key));
      }

      presentation.presetId = *optId;

      if (node.has_child("columnSort"))
      {
        reader.requiredValue("columnSort", presentation.optColumnSort.emplace(), readColumnSort);
      }

      reader.requiredScalar("group", presentation.group)
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
        .scalar("libraryId", utility::formatUuid(document.libraryId))
        .sequence("openViews", document.openViews, writeView)
        .scalar("activeViewIndex", document.activeViewIndex)
        .sequence("customPresets", document.customPresets, writePreset);
      return std::move(writer).finish();
    }

    Result<WorkspaceSessionDocument> readDocument(ryml::ConstNodeRef node)
    {
      constexpr auto kContext = std::string_view{"workspace"};

      if (auto const res = yaml::requireMap(node, kContext); !res)
      {
        return std::unexpected{res.error()};
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

      constexpr auto kKeys = std::to_array<std::string_view>(
        {"presentationVersion", "libraryId", "openViews", "activeViewIndex", "customPresets"});

      auto document = WorkspaceSessionDocument{.presentationVersion = *presentationVersionRes};
      auto reader = yaml::MapReader{node, kKeys, kContext};
      reader.requiredValue("libraryId", document.libraryId, yaml::uuidScalar)
        .requiredSequence("openViews", document.openViews, readView)
        .requiredScalar("activeViewIndex", document.activeViewIndex)
        .requiredSequence("customPresets", document.customPresets, readPreset);
      return std::move(reader).finish(std::move(document));
    }
  } // namespace

  WorkspaceSessionDocument toWorkspaceSessionDocument(WorkspaceSessionState const& state)
  {
    AO_EXPECTS(state.activeViewIndex <= std::numeric_limits<std::uint32_t>::max(),
               "Workspace active view index is not representable");

    AO_EXPECTS(isValidActiveViewIndex(state.activeViewIndex, state.openViews.size()),
               "Workspace active view index is out of bounds");

    auto document = WorkspaceSessionDocument{
      .presentationVersion = kWorkspacePresentationVersion,
      .libraryId = state.libraryId,
      .activeViewIndex = static_cast<std::uint32_t>(state.activeViewIndex),
    };
    document.openViews.reserve(state.openViews.size());

    for (auto const& view : state.openViews)
    {
      AO_EXPECTS(view.listId != kInvalidListId, "Workspace view uses the invalid list id");
      AO_EXPECTS(view.optPresentation, "Workspace view has no exact presentation to persist");

      document.openViews.push_back(StoredTrackListViewConfig{
        .listId = view.listId.raw(),
        .filterExpression = view.filterExpression,
        .presentation = toStoredPresentation(*view.optPresentation),
      });
    }

    document.customPresets.reserve(state.customPresets.size());

    for (auto const& preset : state.customPresets)
    {
      AO_EXPECTS(std::holds_alternative<CustomPresetId>(preset.spec.presetId), "A custom preset requires a custom id");
      AO_EXPECTS(!preset.spec.optColumnSort, "A custom preset definition cannot include a column sort");
      auto spec = toStoredPresentation(preset.spec);

      document.customPresets.push_back(StoredCustomTrackPresentationPreset{
        .label = preset.label,
        .basePresetId = preset.optBasePresetId ? trackPresetKey(*preset.optBasePresetId) : std::string{},
        .spec = std::move(spec),
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

    if (!isValidActiveViewIndex(document.activeViewIndex, document.openViews.size()))
    {
      return makeError(Error::Code::FormatRejected, "Workspace active view index is out of bounds");
    }

    auto state = WorkspaceSessionState{.libraryId = document.libraryId, .activeViewIndex = document.activeViewIndex};
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

      if (specRes->optColumnSort)
      {
        return makeError(Error::Code::FormatRejected, "A custom preset definition cannot include a column sort");
      }

      if (!std::holds_alternative<CustomPresetId>(specRes->presetId))
      {
        return makeError(Error::Code::FormatRejected, "A custom preset requires a custom id");
      }

      auto const optBaseId = parseTrackPresetId(stored.basePresetId);

      if (!optBaseId && !stored.basePresetId.empty())
      {
        return makeError(Error::Code::FormatRejected, std::format("Invalid base preset id '{}'", stored.basePresetId));
      }

      state.customPresets.push_back(CustomTrackPresentationPreset{
        .label = stored.label,
        .optBasePresetId = optBaseId,
        .spec = std::move(*specRes),
      });
    }

    return state;
  }

  Result<> WorkspaceSessionYamlSchema::serialize(ryml::NodeRef node, WorkspaceSessionState const& state) const
  {
    return writeDocument(node, toWorkspaceSessionDocument(state));
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
