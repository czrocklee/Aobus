// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::gtk
{
  inline constexpr std::string_view kTrackOrderMoveUpActionId = "track.orderMoveUp";
  inline constexpr std::string_view kTrackOrderMoveDownActionId = "track.orderMoveDown";
  inline constexpr std::string_view kTrackOrderMoveToTopActionId = "track.orderMoveToTop";
  inline constexpr std::string_view kTrackOrderMoveToBottomActionId = "track.orderMoveToBottom";
  inline constexpr std::string_view kTrackOrderResetActionId = "track.orderReset";
  inline constexpr std::string_view kTrackOrderForgetHiddenActionId = "track.orderForgetHidden";

  constexpr bool isTrackOrderAction(std::string_view const actionId) noexcept
  {
    return actionId == kTrackOrderMoveUpActionId || actionId == kTrackOrderMoveDownActionId ||
           actionId == kTrackOrderMoveToTopActionId || actionId == kTrackOrderMoveToBottomActionId ||
           actionId == kTrackOrderResetActionId || actionId == kTrackOrderForgetHiddenActionId;
  }

  constexpr bool isSinglePressTrackOrderAction(std::string_view const actionId) noexcept
  {
    return actionId == kTrackOrderMoveUpActionId || actionId == kTrackOrderMoveDownActionId ||
           actionId == kTrackOrderMoveToTopActionId || actionId == kTrackOrderMoveToBottomActionId;
  }

  enum class TrackOrderCommand : std::uint8_t
  {
    MoveUp,
    MoveDown,
    MoveToTop,
    MoveToBottom,
    Reset,
    ForgetHidden,
  };
} // namespace ao::gtk
