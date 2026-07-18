// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  inline rt::NotificationEntry entry(
    rt::NotificationId id,
    rt::NotificationSeverity severity,
    std::string message,
    rt::NotificationLifetime lifetime = rt::NotificationLifetime::sessionHistory(),
    rt::NotificationActivityPresentation activityPresentation = rt::NotificationActivityPresentation::Default)
  {
    return rt::NotificationEntry{
      .id = id,
      .severity = severity,
      .message = std::move(message),
      .lifetime = lifetime,
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

  inline rt::NotificationFeedUpdate postedUpdate(rt::NotificationFeedState snapshot, rt::NotificationId const id)
  {
    auto feedPtr = std::make_shared<rt::NotificationFeedState const>(std::move(snapshot));
    return rt::NotificationFeedUpdate{
      .revision = feedPtr->revision,
      .mutationKind = rt::NotificationFeedMutationKind::Posted,
      .affectedIds = {id},
      .feedPtr = std::move(feedPtr),
    };
  }

  inline rt::NotificationFeedUpdate dismissedUpdate(rt::NotificationFeedState snapshot, rt::NotificationId const id)
  {
    auto feedPtr = std::make_shared<rt::NotificationFeedState const>(std::move(snapshot));
    return rt::NotificationFeedUpdate{
      .revision = feedPtr->revision,
      .mutationKind = rt::NotificationFeedMutationKind::Dismissed,
      .affectedIds = {id},
      .feedPtr = std::move(feedPtr),
    };
  }

  inline rt::LibraryChanges::LibraryTaskCompleted libraryTaskCompletion(
    std::size_t affectedCount,
    rt::LibraryChanges::LibraryTaskCompletionStatus status = rt::LibraryChanges::LibraryTaskCompletionStatus::Succeeded)
  {
    return {.status = status, .affectedCount = affectedCount};
  }
} // namespace ao::uimodel::test
