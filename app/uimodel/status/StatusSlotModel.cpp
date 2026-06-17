// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/StateTypes.h>
#include <ao/uimodel/status/StatusSlotModel.h>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::status
{
  namespace
  {
    StatusSlotViewState selectionInfoState()
    {
      return StatusSlotViewState{.mode = StatusSlotDisplayMode::SelectionInfo};
    }

    StatusSlotViewState progressState(std::string message, double fraction)
    {
      return StatusSlotViewState{
        .mode = StatusSlotDisplayMode::Progress,
        .message = std::move(message),
        .progressFraction = fraction,
      };
    }

    StatusSlotViewState notificationState(rt::NotificationEntry const& entry)
    {
      return StatusSlotViewState{
        .mode = StatusSlotDisplayMode::Message,
        .message = entry.message,
        .optSeverity = entry.severity,
        .optAutoDismissTimeout = entry.sticky ? std::optional<std::chrono::milliseconds>{}
                                              : entry.optTimeout.value_or(kStatusSlotDefaultAutoDismissTimeout),
      };
    }

    StatusSlotViewState libraryCompletionState(std::size_t count)
    {
      return StatusSlotViewState{
        .mode = StatusSlotDisplayMode::Message,
        .message =
          count == 0 ? std::string{"Library is up to date"} : std::format("Scan complete: {} tracks added", count),
        .optSeverity = rt::NotificationSeverity::Info,
        .optAutoDismissTimeout = kStatusSlotDefaultAutoDismissTimeout,
      };
    }
  } // namespace

  std::string_view statusSlotSeverityCssClass(rt::NotificationSeverity severity)
  {
    switch (severity)
    {
      case rt::NotificationSeverity::Info: return "ao-status-info";
      case rt::NotificationSeverity::Warning: return "ao-status-warning";
      case rt::NotificationSeverity::Error: return "ao-status-error";
    }

    return {};
  }

  StatusSlotViewState StatusSlotModel::initialState() const
  {
    return selectionInfoState();
  }

  StatusSlotViewState StatusSlotModel::onLibraryTaskProgress(std::string message, double fraction)
  {
    _taskActive = true;
    return progressState(std::move(message), fraction);
  }

  StatusSlotViewState StatusSlotModel::onLibraryTaskCompleted(std::size_t count)
  {
    _taskActive = false;

    if (_optDeferredNotification)
    {
      auto const entry = *_optDeferredNotification;
      _optDeferredNotification.reset();
      return notificationState(entry);
    }

    return libraryCompletionState(count);
  }

  std::optional<StatusSlotViewState> StatusSlotModel::onNotificationPosted(rt::NotificationEntry const& entry)
  {
    if (_taskActive)
    {
      _optDeferredNotification = entry;
      return std::nullopt;
    }

    return notificationState(entry);
  }

  StatusSlotViewState StatusSlotModel::onAutoDismiss()
  {
    return selectionInfoState();
  }
} // namespace ao::uimodel::status
