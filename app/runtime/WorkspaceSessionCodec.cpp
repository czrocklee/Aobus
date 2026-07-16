// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "WorkspaceSessionCodec.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceSessionState.h>

#include <algorithm>
#include <expected>
#include <format>
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

    template<typename Enum, typename IdFunction>
    Result<std::string> encodeEnum(Enum value, IdFunction idFunction, std::string_view context)
    {
      auto const id = idFunction(value);

      if (id.empty())
      {
        return makeError(Error::Code::InvalidState, std::format("Cannot encode invalid {}", context));
      }

      return std::string{id};
    }

    Result<std::vector<std::string>> encodeFields(std::vector<TrackField> const& fields, std::string_view context)
    {
      auto stored = std::vector<std::string>{};
      stored.reserve(fields.size());

      for (auto const field : fields)
      {
        auto encoded = encodeEnum(field, trackFieldId, context);

        if (!encoded)
        {
          return std::unexpected{encoded.error()};
        }

        if (std::ranges::contains(stored, *encoded))
        {
          return makeError(
            Error::Code::InvalidState, std::format("Cannot encode duplicate {} '{}'", context, *encoded));
        }

        stored.push_back(std::move(*encoded));
      }

      return stored;
    }

    Result<StoredTrackPresentationSpec> encodePresentation(TrackPresentationSpec const& spec)
    {
      if (spec.id.empty())
      {
        return makeError(Error::Code::InvalidState, "Cannot encode a presentation with an empty id");
      }

      auto const normalized = normalizeTrackPresentationSpec(spec);
      auto group = encodeEnum(normalized.groupBy, trackGroupKeyId, "track group key");

      if (!group)
      {
        return std::unexpected{group.error()};
      }

      auto stored = StoredTrackPresentationSpec{.id = normalized.id, .group = std::move(*group)};
      stored.sort.reserve(normalized.sortBy.size());

      for (auto const& term : normalized.sortBy)
      {
        auto field = encodeEnum(term.field, trackSortFieldId, "track sort field");

        if (!field)
        {
          return std::unexpected{field.error()};
        }

        if (std::ranges::contains(stored.sort, *field, &StoredTrackSortTerm::field))
        {
          return makeError(Error::Code::InvalidState, std::format("Cannot encode duplicate sort field '{}'", *field));
        }

        stored.sort.push_back(StoredTrackSortTerm{
          .field = std::move(*field),
          .direction = std::string{term.ascending ? kAscending : kDescending},
        });
      }

      auto visibleFields = encodeFields(normalized.visibleFields, "visible field");

      if (!visibleFields)
      {
        return std::unexpected{visibleFields.error()};
      }

      auto redundantFields = encodeFields(normalized.redundantFields, "redundant field");

      if (!redundantFields)
      {
        return std::unexpected{redundantFields.error()};
      }

      stored.visibleFields = std::move(*visibleFields);
      stored.redundantFields = std::move(*redundantFields);
      return stored;
    }

    Result<std::vector<TrackField>> decodeFields(std::vector<std::string> const& stored, std::string_view context)
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

    Result<TrackPresentationSpec> decodePresentation(StoredTrackPresentationSpec const& stored)
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

      auto visibleFields = decodeFields(stored.visibleFields, "visible field");

      if (!visibleFields)
      {
        return std::unexpected{visibleFields.error()};
      }

      if (visibleFields->empty())
      {
        return makeError(Error::Code::FormatRejected, "Track presentation has no visible fields");
      }

      auto redundantFields = decodeFields(stored.redundantFields, "redundant field");

      if (!redundantFields)
      {
        return std::unexpected{redundantFields.error()};
      }

      spec.visibleFields = std::move(*visibleFields);
      spec.redundantFields = std::move(*redundantFields);
      return spec;
    }
  } // namespace

  Result<WorkspaceSessionDocument> encodeWorkspaceSession(WorkspaceSessionState const& state)
  {
    auto document = WorkspaceSessionDocument{
      .presentationVersion = kWorkspacePresentationVersion,
      .activeListId = state.activeListId.raw(),
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

      auto presentation = encodePresentation(*view.optPresentation);

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
      auto spec = encodePresentation(preset.spec);

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

  Result<WorkspaceSessionState> decodeWorkspaceSession(WorkspaceSessionDocument const& document)
  {
    if (document.presentationVersion != kWorkspacePresentationVersion)
    {
      return makeError(Error::Code::FormatRejected,
                       std::format("Unsupported workspace presentation version {}", document.presentationVersion));
    }

    auto state = WorkspaceSessionState{.activeListId = ListId{document.activeListId}};
    state.openViews.reserve(document.openViews.size());

    for (auto const& stored : document.openViews)
    {
      if (stored.listId == kInvalidListId.raw())
      {
        return makeError(Error::Code::FormatRejected, "Workspace view uses the invalid list id");
      }

      auto presentation = decodePresentation(stored.presentation);

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
      auto spec = decodePresentation(stored.spec);

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
} // namespace ao::rt::detail
