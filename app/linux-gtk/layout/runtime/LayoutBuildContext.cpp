// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/LayoutBuildContext.h"

#include <cstdint>

namespace ao::gtk::layout
{
  std::uint64_t LayoutBuildStateView::generation() const noexcept
  {
    if (_hasGenerationOverride)
    {
      return _generation;
    }

    return _runtimeState->componentStateGeneration;
  }

  void LayoutBuildStateView::overrideGeneration(std::uint64_t generation) noexcept
  {
    _generation = generation;
    _hasGenerationOverride = true;
  }
} // namespace ao::gtk::layout
