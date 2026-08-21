// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  class PresentationTextCatalog;

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
    std::string_view errorMessage;
    std::size_t matchCount = 0;
    bool isAllTracks = false;
  };

  struct SmartListEditorViewState final
  {
    std::string name;
    std::string localExpression;

    std::size_t matchCount = 0;
    bool isAllTracks = false;
    std::string previewStatusText;
    std::string errorText;
    std::string membershipEditingText;
    bool hasDirectMembershipEditing = false;
    bool expressionValid = true;
    bool queryInvalid = false;
    bool canSubmit = false;
    bool previewVisible = true;
    bool errorVisible = false;
  };

  SmartListEditorViewState makeSmartListEditorViewState(PresentationTextCatalog const& textCatalog,
                                                        SmartListPreviewState const& input);

  // Expression text

  std::string formatSmartListExpressionDisplayText(PresentationTextCatalog const& textCatalog,
                                                   std::string_view expression);

  std::string combineSmartListEffectiveExpression(std::string_view parent, std::string_view local);

  // Preview

  std::string formatSmartListPreviewStatusText(PresentationTextCatalog const& textCatalog,
                                               bool expressionValid,
                                               std::size_t count,
                                               bool isAllTracks,
                                               bool localEmpty);

  std::string formatSmartListPreviewTrackLabel(PresentationTextCatalog const& textCatalog,
                                               std::string_view title,
                                               std::string_view artist,
                                               std::string_view album);

  rt::LibraryListDraft makeSmartListDraft(ListId parentListId,
                                          ListId editListId,
                                          std::string name,
                                          std::string description,
                                          std::string expression);
} // namespace ao::uimodel
