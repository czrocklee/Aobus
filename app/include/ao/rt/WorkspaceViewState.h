// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CorePrimitives.h"

#include <cstdint>
#include <vector>

namespace ao::rt
{
  struct WorkspaceViewState final
  {
    ViewId activeViewId = kInvalidViewId;
    std::vector<ViewId> openViews{};
    std::uint64_t revision = 0;
  };
} // namespace ao::rt
