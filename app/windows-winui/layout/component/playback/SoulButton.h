// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>

#include <memory>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui::layout
{
  struct LayoutBuildContext;

  /**
   * @brief Build the soul: the animated disc every shell puts at its centre.
   *
   * The soul always reports playback, and additionally drives it when the
   * document leaves its primary click alone. Whichever role it takes, it carries
   * the audio pipeline explanation, because that belongs to the soul rather than
   * to the shell that placed it.
   */
  Result<std::unique_ptr<LayoutComponent>> makeSoulButton(LayoutBuildContext& ctx, uimodel::LayoutNode const& node);
} // namespace ao::winui::layout
