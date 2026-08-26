// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryTaskEvents.h>

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

  rt::NotificationFeedUpdate postedUpdate(rt::NotificationFeedState snapshot, rt::NotificationId id);

  rt::NotificationFeedUpdate expiredUpdate(rt::NotificationFeedState snapshot, rt::NotificationId id);

  inline rt::LibraryTaskProgressUpdated libraryTaskProgress(rt::LibraryTaskProgressKind kind,
                                                            std::string subject,
                                                            double fraction,
                                                            rt::LibraryTaskProgressId id = rt::LibraryTaskProgressId{1})
  {
    return {.id = id, .kind = kind, .fraction = fraction, .subject = std::move(subject)};
  }

  inline rt::LibraryTaskProgressFinished libraryTaskProgressFinished(
    rt::LibraryTaskProgressId id = rt::LibraryTaskProgressId{1})
  {
    return {.id = id};
  }
} // namespace ao::uimodel::test
