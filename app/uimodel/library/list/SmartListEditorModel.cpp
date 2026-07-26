// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/uimodel/library/list/SmartListEditorModel.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  SmartListEditorViewState makeSmartListEditorViewState(SmartListPreviewState const& input)
  {
    auto state = SmartListEditorViewState{};
    state.name = std::string{input.name};
    state.localExpression = std::string{input.localExpression};
    state.matchCount = input.matchCount;
    state.isAllTracks = input.isAllTracks;

    if (!input.hasPreviewSource)
    {
      state.status = SmartListPreviewStatus::PreviewSourceUnavailable;
      state.expressionValid = false;
      state.previewVisible = false;
      state.errorVisible = false;
      state.canSubmit = false;
      return state;
    }

    state.status = input.hasError ? SmartListPreviewStatus::InvalidExpression : SmartListPreviewStatus::Valid;
    state.previewStatusText = formatSmartListPreviewStatusText(
      state.status, input.matchCount, input.isAllTracks, input.localExpression.empty());
    state.queryInvalid = input.hasError && !input.localExpression.empty();
    state.errorVisible = state.queryInvalid;
    state.previewVisible = !state.queryInvalid;
    state.expressionValid = !state.queryInvalid;

    if (state.errorVisible)
    {
      state.errorText = PresentationTextCatalog{}.trackFilterError(input.errorMessage);
    }

    state.canSubmit =
      canSubmitSmartListDraft(input.name, deriveSmartListPreviewStatus(state.expressionValid, input.hasPreviewSource));
    return state;
  }

  std::string formatSmartListExpressionDisplayText(std::string_view expression)
  {
    return expression.empty() ? "(none)" : std::string{expression};
  }

  std::string combineSmartListEffectiveExpression(std::string_view parent, std::string_view local)
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

  SmartListPreviewStatus deriveSmartListPreviewStatus(bool expressionValid, bool hasPreviewSource)
  {
    if (!expressionValid)
    {
      return SmartListPreviewStatus::InvalidExpression;
    }

    if (!hasPreviewSource)
    {
      return SmartListPreviewStatus::PreviewSourceUnavailable;
    }

    return SmartListPreviewStatus::Valid;
  }

  std::string formatSmartListPreviewStatusText(SmartListPreviewStatus status,
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

      return std::format("Showing all {}{}", formatTrackCount(count), isAllTracks ? "" : " from source");
    }

    switch (status)
    {
      case SmartListPreviewStatus::InvalidExpression: return "Invalid filter";
      case SmartListPreviewStatus::PreviewSourceUnavailable: return "No tracks in source";
      case SmartListPreviewStatus::Valid:
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

  std::string formatSmartListPreviewTrackLabel(std::string_view title, std::string_view artist, std::string_view album)
  {
    if (!title.empty())
    {
      auto formatted = std::string{title};

      if (!artist.empty())
      {
        formatted = std::format("{} - {}", formatted, artist);
      }

      if (!album.empty())
      {
        formatted = std::format("{} ({})", formatted, album);
      }

      return formatted;
    }

    if (!artist.empty())
    {
      auto formatted = std::string{artist};

      if (!album.empty())
      {
        formatted = std::format("{} ({})", formatted, album);
      }

      return formatted;
    }

    return "(untitled)";
  }

  bool canSubmitSmartListDraft(std::string_view name, SmartListPreviewStatus status)
  {
    return !name.empty() && (status != SmartListPreviewStatus::InvalidExpression);
  }

  // The draft owns its strings, so they are taken by value and moved in: a
  // caller holding a temporary pays no copy at all.
  rt::LibraryListDraft makeSmartListDraft(ListId parentListId,
                                          ListId editListId,
                                          std::string name,
                                          std::string description,
                                          std::string expression)
  {
    auto draft = rt::LibraryListDraft{};
    draft.kind = rt::LibraryListKind::Smart;
    draft.parentId = parentListId;
    draft.listId = editListId;
    draft.name = std::move(name);
    draft.description = std::move(description);
    draft.expression = std::move(expression);
    return draft;
  }
} // namespace ao::uimodel
