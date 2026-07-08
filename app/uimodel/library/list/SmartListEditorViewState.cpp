// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListDraft.h>
#include <ao/uimodel/library/list/SmartListEditorViewState.h>
#include <ao/uimodel/library/list/SmartListPreview.h>

#include <format>
#include <string>

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
      state.errorText = std::format("Filter error: {}", input.errorMessage);
    }

    state.canSubmit =
      canSubmitSmartListDraft(input.name, deriveSmartListPreviewStatus(state.expressionValid, input.hasPreviewSource));
    return state;
  }
} // namespace ao::uimodel
