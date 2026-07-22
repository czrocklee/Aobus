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
  inline rt::NotificationEntry entry(rt::NotificationId id,
                                     rt::NotificationSeverity severity,
                                     std::string message,
                                     rt::NotificationLifetime lifetime = rt::NotificationLifetime::history())
  {
    return rt::NotificationEntry{
      .id = id,
      .severity = severity,
      .message = std::move(message),
      .lifetime = lifetime,
    };
  }

  inline rt::NotificationFeedState feed(std::vector<rt::NotificationEntry> entries)
  {
    return rt::NotificationFeedState{.entries = std::move(entries)};
  }

  inline rt::NotificationFeedUpdate postedUpdate(rt::NotificationFeedState snapshot, rt::NotificationId const id)
  {
    auto feedPtr = std::make_shared<rt::NotificationFeedState const>(std::move(snapshot));
    return rt::NotificationFeedUpdate{
      .mutationKind = rt::NotificationFeedMutationKind::Posted,
      .id = id,
      .feedPtr = std::move(feedPtr),
    };
  }

  inline rt::NotificationFeedUpdate expiredUpdate(rt::NotificationFeedState snapshot, rt::NotificationId const id)
  {
    auto feedPtr = std::make_shared<rt::NotificationFeedState const>(std::move(snapshot));
    return rt::NotificationFeedUpdate{
      .mutationKind = rt::NotificationFeedMutationKind::Expired,
      .id = id,
      .feedPtr = std::move(feedPtr),
    };
  }

  inline rt::LibraryChanges::LibraryTaskCompleted libraryTaskCompletion(
    std::size_t affectedCount,
    rt::LibraryChanges::LibraryTaskCompletionStatus status = rt::LibraryChanges::LibraryTaskCompletionStatus::Succeeded)
  {
    return {.status = status, .affectedCount = affectedCount};
  }

  inline rt::LibraryChanges::LibraryTaskProgressUpdated
  libraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressKind kind, std::string subject, double fraction)
  {
    return {.kind = kind, .fraction = fraction, .subject = std::move(subject)};
  }
} // namespace ao::uimodel::test
