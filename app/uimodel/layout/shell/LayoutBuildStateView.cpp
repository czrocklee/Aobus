// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>

#include <cstdint>

namespace ao::uimodel
{
  std::uint64_t LayoutBuildStateView::generation() const noexcept
  {
    if (_hasGenerationOverride)
    {
      return _generation;
    }

    return _runtimeState->componentStateGeneration;
  }

  void LayoutBuildStateView::overrideGeneration(std::uint64_t const generation) noexcept
  {
    _generation = generation;
    _hasGenerationOverride = true;
  }
} // namespace ao::uimodel
