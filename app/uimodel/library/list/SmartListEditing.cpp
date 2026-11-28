// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListEditing.h>

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/WritableTagList.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::uimodel
{
  SmartListEditorViewState makeSmartListEditorViewState(i18n::MessageCatalog const& textCatalog,
                                                        SmartListPreviewState const& input)
  {
    auto state = SmartListEditorViewState{};
    state.name = std::string{input.name};
    state.localExpression = std::string{input.localExpression};
    state.isAllTracks = input.isAllTracks;
    auto const optWritableTag = rt::writableTagForListExpression(input.localExpression);
    state.hasDirectMembershipEditing = optWritableTag.has_value();

    if (optWritableTag)
    {
      state.membershipEditingText =
        smartListMembershipEditingText(textCatalog, true, rt::serializeTagExpression(*optWritableTag));
    }
    else
    {
      state.membershipEditingText = smartListMembershipEditingText(textCatalog, false);
    }

    if (!input.hasPreviewSource)
    {
      state.expressionValid = false;
      state.previewVisible = false;
      state.errorVisible = !input.errorMessage.empty();

      if (state.errorVisible)
      {
        state.errorText = i18n::requiredFormat(
          textCatalog, i18n::MessageId::SmartListPreviewUnavailable, {{"diagnostic", input.errorMessage}});
      }

      return state;
    }

    state.queryInvalid = isSmartListLocalFilterRejected(input.localExpression, input.hasError);
    state.errorVisible = state.queryInvalid;
    state.previewVisible = !state.queryInvalid;
    state.expressionValid = !state.queryInvalid;
    state.canSubmit = !state.name.empty() && state.expressionValid;
    state.matchCount = state.expressionValid ? input.matchCount : 0;
    state.previewStatusText = smartListPreviewStatus(textCatalog,
                                                     state.expressionValid,
                                                     state.matchCount,
                                                     input.optSampleCount,
                                                     input.isAllTracks,
                                                     input.localExpression.empty());

    if (state.errorVisible)
    {
      state.errorText =
        i18n::requiredFormat(textCatalog, i18n::MessageId::TrackFilterError, {{"diagnostic", input.errorMessage}});
    }

    return state;
  }

  std::string formatSmartListEditorErrorText(std::string_view mutationError, SmartListEditorViewState const& state)
  {
    auto text = std::string{mutationError};

    if (state.errorVisible && !state.errorText.empty() && state.errorText != mutationError)
    {
      if (!text.empty())
      {
        text += '\n';
      }

      text += state.errorText;
    }

    return text;
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

  std::string formatSmartListPreviewTrackLabel(i18n::MessageCatalog const& textCatalog,
                                               std::string_view title,
                                               std::string_view artist,
                                               std::string_view album)
  {
    if (title.empty() && artist.empty())
    {
      return std::string{i18n::requiredText(textCatalog, i18n::MessageId::SmartListUntitledTrack)};
    }

    auto const primary = title.empty() ? artist : title;
    auto const includeArtist = !title.empty() && !artist.empty();
    // Each suffix adds three delimiter bytes.
    auto const size = primary.size() + (includeArtist ? artist.size() + 3 : 0) + (album.empty() ? 0 : album.size() + 3);
    auto formatted = std::string{};
    formatted.reserve(size);
    formatted.append(primary);

    if (includeArtist)
    {
      formatted.append(" - ").append(artist);
    }

    if (!album.empty())
    {
      formatted.append(" (").append(album).append(")");
    }

    return formatted;
  }

  rt::ListDraft makeSmartListDraft(ListId parentListId,
                                   ListId editListId,
                                   std::string name,
                                   std::string description,
                                   std::string expression)
  {
    return {.parentId = parentListId,
            .listId = editListId,
            .name = std::move(name),
            .description = std::move(description),
            .expression = std::move(expression)};
  }

  std::size_t resolveSmartListTrackPresentationIndex(std::optional<std::string> const& optPresentationId,
                                                     std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                     std::span<rt::CustomTrackPresentationPreset const> customPresets)
  {
    auto const optId = optPresentationId ? rt::parseTrackPresetId(*optPresentationId) : std::nullopt;

    if (!optId)
    {
      return kSmartListAutoTrackPresentationIndex;
    }

    if (auto const* builtin = std::get_if<rt::BuiltinPreset>(&*optId); builtin != nullptr)
    {
      auto const it = std::ranges::find(builtinPresets, *builtin, &rt::TrackPresentationPreset::presetId);
      return it != builtinPresets.end() ? static_cast<std::size_t>(it - builtinPresets.begin()) + 1
                                        : kSmartListAutoTrackPresentationIndex;
    }

    // Custom presentations follow the builtins in the editor's option order.
    auto const customId = std::get<rt::CustomPresetId>(*optId);
    auto const it = std::ranges::find(customPresets, customId, &rt::CustomTrackPresentationPreset::presetId);
    return it != customPresets.end() ? builtinPresets.size() + static_cast<std::size_t>(it - customPresets.begin()) + 1
                                     : kSmartListAutoTrackPresentationIndex;
  }

  std::string resolveSmartListTrackPresentationId(
    std::size_t const selectedIndex,
    bool const selectedIndexValid,
    std::span<rt::TrackPresentationPreset const> const builtinPresets,
    std::span<rt::CustomTrackPresentationPreset const> const customPresets)
  {
    // Auto and an invalid selection are absence of a preference, not today's
    // concrete recommendation: the recommendation happens when a saved Auto
    // list is opened through ListPresentations::presentationForList.
    if (!selectedIndexValid || selectedIndex == kSmartListAutoTrackPresentationIndex)
    {
      return {};
    }

    if (auto const presetIndex = selectedIndex - 1; presetIndex < builtinPresets.size())
    {
      return std::string{rt::builtinPresetKey(builtinPresets[presetIndex].presetId())};
    }

    // The option order places custom presentations after the builtins, so a
    // manual selection there maps back to its custom id instead of the default.
    auto const customIndex = selectedIndex - 1 - builtinPresets.size();

    if (customIndex < customPresets.size())
    {
      return rt::trackPresetKey(customPresets[customIndex].presetId());
    }

    return std::string{rt::builtinPresetKey(rt::BuiltinPreset::Library)};
  }
} // namespace ao::uimodel
