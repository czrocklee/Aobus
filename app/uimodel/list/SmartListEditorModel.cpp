// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/list/SmartListEditorModel.h>
#include <ao/uimodel/track/TrackPresentationRecommender.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::uimodel::list
{
  std::string SmartListEditorModel::displayExpression(std::string_view expression)
  {
    return expression.empty() ? "(none)" : std::string{expression};
  }

  std::string SmartListEditorModel::composeEffectiveExpression(std::string_view parent, std::string_view local)
  {
    if (parent.empty())
    {
      return std::string{local};
    }

    if (local.empty())
    {
      return std::string{parent};
    }

    return std::format("({}) and ({})", parent, local);
  }

  bool SmartListEditorModel::canSubmit(std::string_view name, SmartListStatus status)
  {
    return !name.empty() && (status != SmartListStatus::InvalidExpression);
  }

  SmartListStatus SmartListEditorModel::dialogStatus(bool expressionValid, bool hasPreviewSource)
  {
    if (!expressionValid)
    {
      return SmartListStatus::InvalidExpression;
    }

    if (!hasPreviewSource)
    {
      return SmartListStatus::EmptySource;
    }

    return SmartListStatus::Valid;
  }

  std::string SmartListEditorModel::previewStatusText(SmartListStatus status,
                                                      std::size_t count,
                                                      bool isAllTracks,
                                                      bool localEmpty)
  {
    if (localEmpty)
    {
      if (count == 0)
      {
        return isAllTracks ? "No tracks in library" : "No tracks in source";
      }

      return std::format("Showing all {} {}", count, isAllTracks ? "tracks" : "source tracks");
    }

    switch (status)
    {
      case SmartListStatus::InvalidExpression: return "Invalid filter";
      case SmartListStatus::EmptySource: return "No tracks in source";
      case SmartListStatus::Valid:
      {
        if (count == 0)
        {
          return "No matches";
        }

        constexpr std::size_t kMaxPreview = 10;

        if (count <= kMaxPreview)
        {
          return std::format("Showing all {} matches", count);
        }

        return std::format("Showing {} of {} matches", kMaxPreview, count);
      }
    }

    return "";
  }

  SmartListEditorViewState SmartListEditorModel::previewState(SmartListPreviewInput const& input)
  {
    auto state = SmartListEditorViewState{};
    state.name = std::string{input.name};
    state.localExpression = std::string{input.localExpression};
    state.matchCount = input.matchCount;
    state.isAllTracks = input.isAllTracks;

    if (!input.hasPreviewSource)
    {
      state.status = SmartListStatus::InvalidExpression;
      state.expressionValid = false;
      state.previewVisible = false;
      state.errorVisible = false;
      state.canSubmit = false;
      return state;
    }

    state.status = input.hasError ? SmartListStatus::InvalidExpression : SmartListStatus::Valid;
    state.previewStatusText =
      previewStatusText(state.status, input.matchCount, input.isAllTracks, input.localExpression.empty());
    state.queryInvalid = input.hasError && !input.localExpression.empty();
    state.errorVisible = state.queryInvalid;
    state.previewVisible = !state.queryInvalid;
    state.expressionValid = !state.queryInvalid;

    if (state.errorVisible)
    {
      state.errorText = std::format("Filter error: {}", input.errorMessage);
    }

    state.canSubmit = canSubmit(input.name, dialogStatus(state.expressionValid, input.hasPreviewSource));
    return state;
  }

  std::size_t SmartListEditorModel::presentationIndexForId(std::optional<std::string> const& optPresentationId,
                                                           std::span<rt::TrackPresentationPreset const> builtinPresets)
  {
    if (!optPresentationId)
    {
      return kAutoPresentationIndex;
    }

    auto const it =
      std::ranges::find(builtinPresets, *optPresentationId, [](auto const& preset) { return preset.spec.id; });

    if (it == builtinPresets.end())
    {
      return kAutoPresentationIndex;
    }

    return static_cast<std::size_t>(std::ranges::distance(builtinPresets.begin(), it)) + 1;
  }

  std::string SmartListEditorModel::resolvePresentationId(
    std::size_t selectedIndex,
    bool selectedIndexValid,
    std::string_view localExpression,
    std::span<rt::TrackPresentationPreset const> builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> customPresets)
  {
    if (!selectedIndexValid || selectedIndex == kAutoPresentationIndex)
    {
      return ao::uimodel::track::recommendPresentation(localExpression, builtinPresets, customPresets).id;
    }

    if (auto const presetIndex = selectedIndex - 1; presetIndex < builtinPresets.size())
    {
      return std::string{builtinPresets[presetIndex].spec.id};
    }

    return std::string{rt::kDefaultTrackPresentationId};
  }

  rt::LibraryWriter::ListDraft SmartListEditorModel::createDraft(ListId parentListId,
                                                                 ListId editListId,
                                                                 std::string const& name,
                                                                 std::string const& description,
                                                                 std::string const& expression)
  {
    auto draftData = rt::LibraryWriter::ListDraft{};
    draftData.kind = rt::LibraryWriter::ListKind::Smart;
    draftData.parentId = parentListId;
    draftData.listId = editListId;
    draftData.name = name;
    draftData.description = description;
    draftData.expression = expression;
    return draftData;
  }
} // namespace ao::uimodel::list
