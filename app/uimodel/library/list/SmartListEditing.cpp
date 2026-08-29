// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListEditing.h>

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  SmartListEditorViewState makeSmartListEditorViewState(i18n::MessageCatalog const& textCatalog,
                                                        SmartListPreviewState const& input)
  {
    auto state = SmartListEditorViewState{};
    state.name = std::string{input.name};
    state.localExpression = std::string{input.localExpression};
    state.matchCount = input.matchCount;
    state.isAllTracks = input.isAllTracks;
    auto const optWritableTag = rt::writableTagForListExpression(input.localExpression);
    state.hasDirectMembershipEditing = optWritableTag.has_value();

    if (optWritableTag)
    {
      auto const expression =
        query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = *optWritableTag});
      state.membershipEditingText = smartListMembershipEditingText(textCatalog, true, expression);
    }
    else
    {
      state.membershipEditingText = smartListMembershipEditingText(textCatalog, false);
    }

    if (!input.hasPreviewSource)
    {
      state.expressionValid = false;
      state.previewVisible = false;
      state.errorVisible = false;
      return state;
    }

    state.queryInvalid = input.hasError && !input.localExpression.empty();
    state.errorVisible = state.queryInvalid;
    state.previewVisible = !state.queryInvalid;
    state.expressionValid = !state.queryInvalid;
    state.canSubmit = !state.name.empty() && state.expressionValid;
    state.previewStatusText = formatSmartListPreviewStatusText(
      textCatalog, state.expressionValid, input.matchCount, input.isAllTracks, input.localExpression.empty());

    if (state.errorVisible)
    {
      state.errorText =
        i18n::requiredFormat(textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", input.errorMessage}});
    }

    return state;
  }

  std::string formatSmartListExpressionDisplayText(i18n::MessageCatalog const& textCatalog, std::string_view expression)
  {
    return expression.empty() ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::SmartListExpressionNone)}
                              : std::string{expression};
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

  std::string formatSmartListPreviewStatusText(i18n::MessageCatalog const& textCatalog,
                                               bool const expressionValid,
                                               std::size_t count,
                                               bool isAllTracks,
                                               bool localEmpty)
  {
    return smartListPreviewStatus(textCatalog, expressionValid, count, isAllTracks, localEmpty);
  }

  std::string formatSmartListPreviewTrackLabel(i18n::MessageCatalog const& textCatalog,
                                               std::string_view title,
                                               std::string_view artist,
                                               std::string_view album)
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

    return std::string{i18n::requiredText(textCatalog, i18n::MessageId::SmartListUntitledTrack)};
  }

  // The draft owns its strings, so they are taken by value and moved in: a
  // caller holding a temporary pays no copy at all.
  rt::ListDraft makeSmartListDraft(ListId parentListId,
                                   ListId editListId,
                                   std::string name,
                                   std::string description,
                                   std::string expression)
  {
    auto draft = rt::ListDraft{};
    draft.parentId = parentListId;
    draft.listId = editListId;
    draft.name = std::move(name);
    draft.description = std::move(description);
    draft.expression = std::move(expression);
    return draft;
  }

  std::size_t resolveSmartListTrackPresentationIndex(std::optional<std::string> const& optPresentationId,
                                                     std::span<rt::TrackPresentationPreset const> builtinPresets)
  {
    if (!optPresentationId)
    {
      return kSmartListAutoTrackPresentationIndex;
    }

    auto const it =
      std::ranges::find(builtinPresets, *optPresentationId, [](auto const& preset) { return preset.spec.id; });

    if (it == builtinPresets.end())
    {
      return kSmartListAutoTrackPresentationIndex;
    }

    return static_cast<std::size_t>(std::ranges::distance(builtinPresets.begin(), it)) + 1;
  }

  std::string resolveSmartListTrackPresentationId(
    std::size_t const selectedIndex,
    bool const selectedIndexValid,
    std::string_view const localExpression,
    std::span<rt::TrackPresentationPreset const> const builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> const customPresets)
  {
    if (!selectedIndexValid || selectedIndex == kSmartListAutoTrackPresentationIndex)
    {
      auto const context = ListPresentationContext{
        .sourceKind = ListPresentationSourceKind::SavedList,
        .listExpression = localExpression,
      };
      return recommendListPresentation(context, builtinPresets, customPresets).id;
    }

    if (auto const presetIndex = selectedIndex - 1; presetIndex < builtinPresets.size())
    {
      return std::string{builtinPresets[presetIndex].spec.id};
    }

    return std::string{rt::kDefaultTrackPresentationId};
  }
} // namespace ao::uimodel
