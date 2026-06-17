// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/StateTypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel::status
{
  enum class StatusSlotDisplayMode : std::uint8_t
  {
    SelectionInfo,
    Progress,
    Message,
  };

  struct StatusSlotViewState final
  {
    StatusSlotDisplayMode mode = StatusSlotDisplayMode::SelectionInfo;
    std::string message{};
    double progressFraction = 0.0;
    std::optional<rt::NotificationSeverity> optSeverity{};
    std::optional<std::chrono::milliseconds> optAutoDismissTimeout{};
  };

  inline constexpr std::chrono::milliseconds kStatusSlotDefaultAutoDismissTimeout{5000};

  std::string_view statusSlotSeverityCssClass(rt::NotificationSeverity severity);

  class StatusSlotModel final
  {
  public:
    StatusSlotViewState initialState() const;
    StatusSlotViewState onLibraryTaskProgress(std::string message, double fraction);
    StatusSlotViewState onLibraryTaskCompleted(std::size_t count);
    std::optional<StatusSlotViewState> onNotificationPosted(rt::NotificationEntry const& entry);
    StatusSlotViewState onAutoDismiss();

  private:
    bool _taskActive = false;
    std::optional<rt::NotificationEntry> _optDeferredNotification{};
  };
} // namespace ao::uimodel::status
