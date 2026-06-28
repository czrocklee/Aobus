// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackPresentation.h>

#include <span>
#include <string_view>

namespace ao::uimodel
{
  /**
   * @brief Recommends a track presentation based on a query filter expression.
   *
   * This is used by smart lists when their presentation preference is resolved
   * from their filter expression.
   *
   * @param filterExpression The smart list filter expression.
   * @param builtinPresets Available builtin presentation presets.
   * @param customPresets Available custom presentation presets.
   * @return A presentation spec derived from the available presets.
   */
  rt::TrackPresentationSpec recommendPresentation(std::string_view filterExpression,
                                                  std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                  std::span<rt::CustomTrackPresentationPreset const> customPresets);
} // namespace ao::uimodel
