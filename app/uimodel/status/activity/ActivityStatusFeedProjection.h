// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
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
    void handleFeedUpdated(rt::NotificationFeedUpdate const& update);
    void handleLibraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressUpdated const& event);
    void handleLibraryTaskCompleted(rt::LibraryChanges::LibraryTaskCompleted const& event,
                                    rt::NotificationFeedState const& feed);
    void dismissCompact(rt::NotificationFeedState const& feed);
    void hideDetailNotification(rt::NotificationId id, rt::NotificationFeedState const& feed);
    void autoDismissCompact(rt::NotificationFeedState const& feed);

    ActivityStatusViewState const& viewState() const noexcept;

  private:
    struct LibraryProgressState final
    {
      rt::LibraryChanges::LibraryTaskProgressKind kind = rt::LibraryChanges::LibraryTaskProgressKind::Scanning;
      std::string subject{};
      double fraction = 0.0;
    };

    void handleFeedChanged(rt::NotificationFeedState const& feed);
    void handleNotificationPosted(rt::NotificationFeedState const& feed, rt::NotificationId id);
    bool refreshesVisibleTransient(rt::NotificationFeedUpdate const& update) const;
    void projectDetail(rt::NotificationFeedState const& feed);
    void projectPersistentCompact(rt::NotificationFeedState const& feed);
    void projectNotificationCompact(rt::NotificationEntry const& entry);
    void projectCompletionCompact(std::size_t count);
    void setCompact(ActivityCompactState compact, std::vector<rt::NotificationId> sourceIds = {});
    bool hasPresentedCompactSource(std::vector<rt::NotificationId> const& sourceIds,
                                   rt::NotificationFeedState const& feed) const;
    bool areCompactSourcesHidden(std::vector<rt::NotificationId> const& sourceIds) const;
    bool isCompactSourceHidden(rt::NotificationId id) const;
    void rememberHiddenCompactSources();
    void pruneHiddenSources(rt::NotificationFeedState const& feed);

    ActivityStatusViewState _state{};
    PresentationTextCatalog _textCatalog{};
    bool _taskActive = false;
    std::optional<LibraryProgressState> _optLibraryProgress{};
    std::vector<rt::NotificationId> _compactSourceNotificationIds{};
    std::vector<rt::NotificationId> _hiddenCompactIds{};
    std::vector<rt::NotificationId> _hiddenDetailIds{};
  };
} // namespace ao::uimodel
