// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/FocusedDetail.h"

#include "pch.h"
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/projection/TrackDetailProjection.h>

#include <memory>

namespace ao::winui::layout
{
  std::shared_ptr<rt::TrackDetailProjection> FocusedDetail::projection(rt::WorkspaceService& workspace)
  {
    if (_projectionPtr == nullptr)
    {
      _projectionPtr = workspace.detailProjection(rt::FocusedViewTarget{});
    }

    return _projectionPtr;
  }
} // namespace ao::winui::layout
