// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel::track
{
  enum class TrackFilterMode : std::uint8_t
  {
    None,
    Quick,
    Expression,
  };

  struct ResolvedTrackFilter final
  {
    TrackFilterMode mode = TrackFilterMode::None;
    std::string expression{};
  };

  /**
   * Resolve raw filter text into a filter mode and expression.
   *
   * Supports:
   * - Empty input (None)
   * - Quoted/multiple quick search terms (Quick)
   * - Complex expressions starting with $ or @ (Expression)
   */
  ResolvedTrackFilter resolveTrackFilterExpression(std::string_view rawFilter);
} // namespace ao::uimodel::track
