// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  struct CustomTrackPresentationPreset;
  struct TrackPresentationPreset;
}

namespace ao::uimodel
{
  /**
   * Saved-List authoring: the editor's expression handling, live preview, view
   * state, and the draft it finally submits.
   *
   * A frontend dialog owns the widgets and feeds observations in through
   * `SmartListPreviewState`; every decision about what the user may see or
   * submit is made here.
   */

  // What the editor currently observes. Borrowed strings: valid only for the
  // duration of the call that consumes this.
  struct SmartListPreviewState final
  {
    std::string_view name;
    std::string_view localExpression;
    bool hasPreviewSource = false;
    bool hasError = false;
    // Source acquisition diagnostic if unavailable; otherwise the filter diagnostic.
    std::string_view errorMessage{};
    std::size_t matchCount = 0;
    // Rows actually provided, before viewport clipping. A count-only host leaves this absent.
    std::optional<std::size_t> optSampleCount{};
    bool isAllTracks = false;
  };

  struct SmartListEditorViewState final
  {
    std::string name{};
    std::string localExpression{};

    std::size_t matchCount = 0;
    bool isAllTracks = false;
    std::string previewStatusText{};
    std::string errorText{};
    std::string membershipEditingText{};
    bool hasDirectMembershipEditing = false;
    bool expressionValid = true;
    bool queryInvalid = false;
    bool canSubmit = false;
    bool previewVisible = true;
    bool errorVisible = false;
  };

  // A source error inherited by a blank draft is not a rejection of its local filter.
  constexpr bool isSmartListLocalFilterRejected(std::string_view localExpression, bool hasSourceError) noexcept
  {
    return hasSourceError && !localExpression.empty();
  }

  SmartListEditorViewState makeSmartListEditorViewState(i18n::MessageCatalog const& textCatalog,
                                                        SmartListPreviewState const& input);

  // Combine display-ready diagnostics, writer first and any distinct visible preview error on its own line.
  std::string formatSmartListEditorErrorText(std::string_view mutationError, SmartListEditorViewState const& state);

  // Expression text

  std::string formatSmartListExpressionDisplayText(i18n::MessageCatalog const& textCatalog,
                                                   std::string_view expression);

  std::string combineSmartListEffectiveExpression(std::string_view parent, std::string_view local);

  // Preview

  /// Maximum leading rows to sample; not a match count or a viewport size.
  inline constexpr std::size_t kSmartListPreviewLimit = 10;

  std::string formatSmartListPreviewTrackLabel(i18n::MessageCatalog const& textCatalog,
                                               std::string_view title,
                                               std::string_view artist,
                                               std::string_view album);

  rt::ListDraft makeSmartListDraft(ListId parentListId,
                                   ListId editListId,
                                   std::string name,
                                   std::string description,
                                   std::string expression);

  inline constexpr std::size_t kSmartListAutoTrackPresentationIndex = 0;

  /**
   * The editor's presentation option order: index 0 is Auto, the next
   * builtinPresets.size() indexes select the builtin presets in span order,
   * and the indexes after that select the custom presets in span order.
   * An absent, empty, or unknown stored id maps to Auto; an editor preserving
   * an unknown id must represent that unavailable choice separately.
   */
  std::size_t resolveSmartListTrackPresentationIndex(std::optional<std::string> const& optPresentationId,
                                                     std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                     std::span<rt::CustomTrackPresentationPreset const> customPresets);

  /**
   * Maps a selected option index back to a presentation id using the option
   * order documented on resolveSmartListTrackPresentationIndex. Auto and an
   * invalid selection resolve to the empty id, absence of a preference; a
   * builtin or custom index resolves to that preset's id; any other index
   * falls back to the default presentation. The recommendation for a saved
   * Auto list belongs to ListPresentations::presentationForList when the
   * view opens, not to the editor's saved preference.
   */
  std::string resolveSmartListTrackPresentationId(std::size_t selectedIndex,
                                                  bool selectedIndexValid,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::uimodel
