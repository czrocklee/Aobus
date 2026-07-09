// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ao::uimodel
{
  class ActivityStatusFeedProjection final
  {
  public:
    void initialize(rt::NotificationFeedState const& feed);
    void handleFeedChanged(rt::NotificationFeedState const& feed);
    void handleNotificationPosted(rt::NotificationFeedState const& feed, rt::NotificationId id);
    void handleLibraryTaskProgress(std::string message, double fraction);
    void handleLibraryTaskCompleted(std::size_t count, rt::NotificationFeedState const& feed);
    void dismissCompact(rt::NotificationFeedState const& feed);
    void dismissDetailNotificationFromActivity(rt::NotificationId id, rt::NotificationFeedState const& feed);
    void handleTransientExpired(rt::NotificationFeedState const& feed);

    std::vector<rt::NotificationId> locallyHideableNotificationIds(rt::NotificationFeedState const& feed) const;
    ActivityStatusViewState const& viewState() const noexcept;

  private:
    struct LibraryProgressState final
    {
      std::string message{};
      double fraction = 0.0;
    };

    void projectDetail(rt::NotificationFeedState const& feed);
    void projectPersistentCompact(rt::NotificationFeedState const& feed);
    void projectNotificationCompact(rt::NotificationEntry const& entry);
    void projectCompletionCompact(std::size_t count);
    bool hasPresentedCompactSource(ActivityCompactState const& compact, rt::NotificationFeedState const& feed) const;
    bool isCompactSourceDismissed(ActivityCompactState const& compact) const;
    bool isCompactSourceSuppressed(rt::NotificationId id) const;
    void rememberDismissedCompactSources();
    void pruneDismissedSources(rt::NotificationFeedState const& feed);

    ActivityStatusViewState _state{};
    bool _taskActive = false;
    std::optional<LibraryProgressState> _optLibraryProgress{};
    std::optional<rt::NotificationEntry> _optDeferredNotification{};
    std::vector<rt::NotificationId> _compactDismissedNotificationIds{};
    std::vector<rt::NotificationId> _detailDismissedNotificationIds{};
  };
} // namespace ao::uimodel
