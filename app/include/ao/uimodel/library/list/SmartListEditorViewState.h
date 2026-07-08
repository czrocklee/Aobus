// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/library/list/SmartListPreview.h>

#include <cstddef>
#include <string>

namespace ao::uimodel
{
  struct SmartListEditorViewState final
  {
    std::string name;
    std::string description;
    std::string localExpression;
    std::string effectiveExpression;

    SmartListPreviewStatus status = SmartListPreviewStatus::Valid;
    std::size_t matchCount = 0;
    bool isAllTracks = false;
    std::string previewStatusText;
    std::string errorText;
    bool expressionValid = true;
    bool queryInvalid = false;
    bool previewVisible = true;
    bool errorVisible = false;
    bool canSubmit = false;
  };

  SmartListEditorViewState makeSmartListEditorViewState(SmartListPreviewState const& input);
} // namespace ao::uimodel
