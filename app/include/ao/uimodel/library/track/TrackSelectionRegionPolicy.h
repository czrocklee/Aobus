// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/projection/ProjectionTypes.h>

#include <string_view>

namespace ao::uimodel
{
  struct TrackSelectionRegionPresentation final
  {
    bool visible = false;
    bool sensitive = false;
  };

  constexpr TrackSelectionRegionPresentation presentationForTrackSelectionRegion(rt::SelectionKind const selectionKind,
                                                                                 std::string_view const showWhen,
                                                                                 bool const showPlaceholder)
  {
    bool const hasSelection = selectionKind != rt::SelectionKind::None;
    bool visible = false;

    if (showWhen == "none")
    {
      visible = selectionKind == rt::SelectionKind::None;
    }
    else if (showWhen == "single")
    {
      visible = selectionKind == rt::SelectionKind::Single;
    }
    else if (showWhen == "multiple")
    {
      visible = selectionKind == rt::SelectionKind::Multiple;
    }
    else if (showWhen == "any")
    {
      visible = hasSelection || showPlaceholder;
    }

    return TrackSelectionRegionPresentation{.visible = visible, .sensitive = hasSelection || !showPlaceholder};
  }
} // namespace ao::uimodel
