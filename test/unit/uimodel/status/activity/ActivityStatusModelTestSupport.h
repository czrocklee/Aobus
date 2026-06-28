// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  inline rt::NotificationEntry entry(
    rt::NotificationId id,
    rt::NotificationSeverity severity,
    std::string message,
    bool sticky = false,
    std::optional<std::chrono::milliseconds> optTimeout = std::nullopt,
    rt::NotificationActivityPresentation activityPresentation = rt::NotificationActivityPresentation::Default)
  {
    return rt::NotificationEntry{
      .id = id,
      .severity = severity,
      .message = std::move(message),
      .sticky = sticky,
      .optTimeout = optTimeout,
      .activityPresentation = activityPresentation,
    };
  }

  inline rt::NotificationEntry progressEntry(rt::NotificationId id, std::string message, double fraction)
  {
    auto result = entry(id, rt::NotificationSeverity::Info, std::move(message));
    result.content.optProgress = rt::NotificationProgressState{
      .mode = rt::NotificationProgressMode::Fraction, .fraction = fraction, .label = "Importing"};
    return result;
  }

  inline rt::NotificationFeedState feed(std::vector<rt::NotificationEntry> entries)
  {
    return rt::NotificationFeedState{.entries = std::move(entries), .revision = 9};
  }
} // namespace ao::uimodel::test
