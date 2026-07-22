// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActivityStatusFeedProjection.h"

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::int32_t severityRank(rt::NotificationSeverity const severity)
    {
      switch (severity)
      {
        case rt::NotificationSeverity::Info: return 0;
        case rt::NotificationSeverity::Warning: return 1;
        case rt::NotificationSeverity::Error: return 2;
      }

      return -1;
    }

    ActivityStatusKind kindForSeverity(rt::NotificationSeverity const severity)
    {
      switch (severity)
      {
        case rt::NotificationSeverity::Info: return ActivityStatusKind::Info;
        case rt::NotificationSeverity::Warning: return ActivityStatusKind::Warning;
        case rt::NotificationSeverity::Error: return ActivityStatusKind::Error;
      }

      return ActivityStatusKind::Info;
    }

    bool isPersistentSeverity(rt::NotificationSeverity const severity)
    {
      return severity == rt::NotificationSeverity::Warning || severity == rt::NotificationSeverity::Error;
    }

    bool isPersistentCompact(ActivityStatusKind const kind)
    {
      return kind == ActivityStatusKind::Warning || kind == ActivityStatusKind::Error;
    }

    bool isDetailEligible(rt::NotificationEntry const& entry)
    {
      return isPersistentSeverity(entry.severity) || entry.lifetime.kind() == rt::NotificationLifetimeKind::Pinned;
    }

    bool isDetailClearable(rt::NotificationEntry const& entry)
    {
      return isDetailEligible(entry) && entry.lifetime.kind() != rt::NotificationLifetimeKind::Pinned;
    }

    std::string primaryText(PresentationTextCatalog const& textCatalog, rt::NotificationEntry const& entry)
    {
      return textCatalog.notificationMessage(entry.message);
    }

    std::string groupedText(rt::NotificationSeverity const severity, std::size_t const count)
    {
      if (count == 1)
      {
        return {};
      }

      switch (severity)
      {
        case rt::NotificationSeverity::Info: return std::format("{} notifications", count);
        case rt::NotificationSeverity::Warning: return std::format("{} warnings", count);
        case rt::NotificationSeverity::Error: return std::format("{} errors", count);
      }

      return std::format("{} notifications", count);
    }

    ActivityDetailItem detailItem(PresentationTextCatalog const& textCatalog, rt::NotificationEntry const& entry)
    {
      return ActivityDetailItem{
        .id = entry.id,
        .severity = entry.severity,
        .message = textCatalog.notificationMessage(entry.message),
        .dismissible = isDetailClearable(entry),
      };
    }
  } // namespace

  bool hasDetailContent(ActivityDetailState const& detail) noexcept
  {
    return !detail.items.empty() || detail.optLibraryTask;
  }

  void ActivityStatusFeedProjection::initialize(rt::NotificationFeedState const& feed)
  {
    projectDetail(feed);
    projectPersistentCompact(feed);
  }

  void ActivityStatusFeedProjection::handleFeedUpdated(rt::NotificationFeedUpdate const& update)
  {
    if (!update.feedPtr)
    {
      return;
    }

    auto const& feed = *update.feedPtr;

    if (update.mutationKind == rt::NotificationFeedMutationKind::Posted)
    {
      handleNotificationPosted(feed, update.id);
      return;
    }

    if (refreshesVisibleTransient(update))
    {
      auto const sourceId = _compactSourceNotificationIds.front();
      auto const iter = std::ranges::find(feed.entries, sourceId, &rt::NotificationEntry::id);

      if (iter != feed.entries.end())
      {
        projectDetail(feed);
        projectNotificationCompact(*iter);
        return;
      }
    }

    handleFeedChanged(feed);
  }

  void ActivityStatusFeedProjection::handleFeedChanged(rt::NotificationFeedState const& feed)
  {
    projectDetail(feed);

    if (_taskActive)
    {
      return;
    }

    if (auto const persistentKind = isPersistentCompact(_state.compact.kind);
        persistentKind && hasPresentedCompactSource(_compactSourceNotificationIds, feed) &&
        !areCompactSourcesHidden(_compactSourceNotificationIds))
    {
      projectPersistentCompact(feed);
      return;
    }

    if (isPersistentCompact(_state.compact.kind) || _state.compact.kind == ActivityStatusKind::Idle)
    {
      projectPersistentCompact(feed);
      return;
    }

    if (auto previousCompact = _state.compact;
        previousCompact.kind == ActivityStatusKind::Info || previousCompact.kind == ActivityStatusKind::Success)
    {
      auto previousSourceIds = _compactSourceNotificationIds;
      projectPersistentCompact(feed);

      bool const sourceStillValid = previousSourceIds.empty() || (hasPresentedCompactSource(previousSourceIds, feed) &&
                                                                  !areCompactSourcesHidden(previousSourceIds));

      if (_state.compact.kind == ActivityStatusKind::Idle && sourceStillValid)
      {
        setCompact(std::move(previousCompact), std::move(previousSourceIds));
      }
    }
  }

  void ActivityStatusFeedProjection::handleNotificationPosted(rt::NotificationFeedState const& feed,
                                                              rt::NotificationId const id)
  {
    projectDetail(feed);

    auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);

    if (iter == feed.entries.end())
    {
      return;
    }

    if (_taskActive)
    {
      return;
    }

    if (isPersistentSeverity(iter->severity))
    {
      projectPersistentCompact(feed);
      return;
    }

    projectPersistentCompact(feed);

    if (_state.compact.kind == ActivityStatusKind::Idle)
    {
      projectNotificationCompact(*iter);
    }
  }

  bool ActivityStatusFeedProjection::refreshesVisibleTransient(rt::NotificationFeedUpdate const& update) const
  {
    if (_taskActive || _state.compact.kind != ActivityStatusKind::Info || _compactSourceNotificationIds.size() != 1)
    {
      return false;
    }

    switch (update.mutationKind)
    {
      case rt::NotificationFeedMutationKind::ReportUpdated: return update.id == _compactSourceNotificationIds.front();
      case rt::NotificationFeedMutationKind::Posted:
      case rt::NotificationFeedMutationKind::Expired: return false;
    }

    return false;
  }

  void ActivityStatusFeedProjection::handleLibraryTaskProgress(
    rt::LibraryChanges::LibraryTaskProgressUpdated const& event)
  {
    _taskActive = true;
    _optLibraryProgress =
      LibraryProgressState{.kind = event.kind, .subject = event.subject, .fraction = event.fraction};
    setCompact(ActivityCompactState{
      .kind = ActivityStatusKind::Processing,
      .text = _textCatalog.libraryTaskProgressCompact(event.kind, event.subject),
      .optProgressFraction = event.fraction,
    });
    _state.detail.optLibraryTask = ActivityTaskDetail{
      .message = _textCatalog.libraryTaskProgressDetail(event.kind, event.subject),
      .progressFraction = event.fraction,
    };
  }

  void ActivityStatusFeedProjection::handleLibraryTaskCompleted(rt::LibraryChanges::LibraryTaskCompleted const& event,
                                                                rt::NotificationFeedState const& feed)
  {
    _taskActive = false;
    _optLibraryProgress.reset();
    projectDetail(feed);

    projectPersistentCompact(feed);

    if (_state.compact.kind == ActivityStatusKind::Idle &&
        event.status == rt::LibraryChanges::LibraryTaskCompletionStatus::Succeeded)
    {
      projectCompletionCompact(event.affectedCount);
    }
  }

  void ActivityStatusFeedProjection::dismissCompact(rt::NotificationFeedState const& feed)
  {
    rememberHiddenCompactSources();
    setCompact({});
    projectDetail(feed);
  }

  void ActivityStatusFeedProjection::hideDetailNotification(rt::NotificationId const id,
                                                            rt::NotificationFeedState const& feed)
  {
    auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);

    if (iter == feed.entries.end() || !isDetailClearable(*iter))
    {
      return;
    }

    if (!std::ranges::contains(_hiddenDetailIds, id))
    {
      _hiddenDetailIds.push_back(id);
    }

    bool const compactReferencedHiddenSource = std::ranges::contains(_compactSourceNotificationIds, id);
    projectDetail(feed);

    if (compactReferencedHiddenSource && !_taskActive)
    {
      projectPersistentCompact(feed);
    }
  }

  void ActivityStatusFeedProjection::autoDismissCompact(rt::NotificationFeedState const& feed)
  {
    projectDetail(feed);
    projectPersistentCompact(feed);
  }

  ActivityStatusViewState const& ActivityStatusFeedProjection::viewState() const noexcept
  {
    return _state;
  }

  void ActivityStatusFeedProjection::projectDetail(rt::NotificationFeedState const& feed)
  {
    pruneHiddenSources(feed);

    auto items = std::vector<ActivityDetailItem>{};
    items.reserve(feed.entries.size());

    for (auto const& entry : feed.entries)
    {
      if (!isDetailEligible(entry))
      {
        continue;
      }

      if (std::ranges::contains(_hiddenDetailIds, entry.id))
      {
        continue;
      }

      items.push_back(detailItem(_textCatalog, entry));
    }

    std::ranges::sort(items, [](auto const& lhs, auto const& rhs) { return lhs.id > rhs.id; });

    auto optLibraryTask = std::optional<ActivityTaskDetail>{};

    if (_optLibraryProgress)
    {
      optLibraryTask = ActivityTaskDetail{
        .message = _textCatalog.libraryTaskProgressDetail(_optLibraryProgress->kind, _optLibraryProgress->subject),
        .progressFraction = _optLibraryProgress->fraction,
      };
    }

    _state.detail = ActivityDetailState{.items = std::move(items), .optLibraryTask = std::move(optLibraryTask)};
  }

  void ActivityStatusFeedProjection::projectPersistentCompact(rt::NotificationFeedState const& feed)
  {
    auto optSeverity = std::optional<rt::NotificationSeverity>{};
    auto ids = std::vector<rt::NotificationId>{};
    rt::NotificationEntry const* latestEntry = nullptr;

    for (auto const& entry : feed.entries)
    {
      if (!isPersistentSeverity(entry.severity))
      {
        continue;
      }

      if (isCompactSourceHidden(entry.id))
      {
        continue;
      }

      if (!optSeverity || severityRank(entry.severity) > severityRank(*optSeverity))
      {
        optSeverity = entry.severity;
        ids.clear();
        latestEntry = &entry;
      }

      if (optSeverity == entry.severity)
      {
        ids.push_back(entry.id);

        if (latestEntry == nullptr || entry.id > latestEntry->id)
        {
          latestEntry = &entry;
        }
      }
    }

    if (!optSeverity || latestEntry == nullptr)
    {
      setCompact({});
      return;
    }

    auto text = groupedText(*optSeverity, ids.size());

    if (text.empty())
    {
      text = primaryText(_textCatalog, *latestEntry);
    }

    auto const hasDetails = !ids.empty();
    setCompact(
      ActivityCompactState{
        .kind = kindForSeverity(*optSeverity),
        .text = std::move(text),
        .dismissible = true,
        .hasDetails = hasDetails,
      },
      std::move(ids));
  }

  void ActivityStatusFeedProjection::projectNotificationCompact(rt::NotificationEntry const& entry)
  {
    setCompact(
      ActivityCompactState{
        .kind = kindForSeverity(entry.severity),
        .text = primaryText(_textCatalog, entry),
        .dismissible = true,
        .hasDetails = isDetailEligible(entry),
        .optAutoDismissTimeout =
          isPersistentSeverity(entry.severity) || entry.lifetime.kind() == rt::NotificationLifetimeKind::Transient
            ? std::optional<std::chrono::milliseconds>{}
            : std::optional{kActivityStatusDefaultAutoDismissTimeout},
      },
      {entry.id});
  }

  void ActivityStatusFeedProjection::projectCompletionCompact(std::size_t const count)
  {
    setCompact(ActivityCompactState{
      .kind = ActivityStatusKind::Success,
      .text = count == 0 ? std::string{"Library is up to date"}
                         : std::format("Scan complete: {} added", formatTrackCount(count)),
      .optAutoDismissTimeout = kActivityStatusDefaultAutoDismissTimeout,
    });
  }

  void ActivityStatusFeedProjection::setCompact(ActivityCompactState compact, std::vector<rt::NotificationId> sourceIds)
  {
    _state.compact = std::move(compact);
    _compactSourceNotificationIds = std::move(sourceIds);
  }

  bool ActivityStatusFeedProjection::hasPresentedCompactSource(std::vector<rt::NotificationId> const& sourceIds,
                                                               rt::NotificationFeedState const& feed) const
  {
    return std::ranges::any_of(sourceIds,
                               [&feed](auto const id)
                               {
                                 auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);
                                 return iter != feed.entries.end();
                               });
  }

  bool ActivityStatusFeedProjection::areCompactSourcesHidden(std::vector<rt::NotificationId> const& sourceIds) const
  {
    return !sourceIds.empty() &&
           std::ranges::all_of(sourceIds, [this](auto const id) { return isCompactSourceHidden(id); });
  }

  bool ActivityStatusFeedProjection::isCompactSourceHidden(rt::NotificationId const id) const
  {
    return std::ranges::contains(_hiddenCompactIds, id) || std::ranges::contains(_hiddenDetailIds, id);
  }

  void ActivityStatusFeedProjection::rememberHiddenCompactSources()
  {
    for (auto const id : _compactSourceNotificationIds)
    {
      if (!std::ranges::contains(_hiddenCompactIds, id))
      {
        _hiddenCompactIds.push_back(id);
      }
    }
  }

  void ActivityStatusFeedProjection::pruneHiddenSources(rt::NotificationFeedState const& feed)
  {
    auto const existsInFeed = [&feed](auto const id)
    { return std::ranges::contains(feed.entries, id, &rt::NotificationEntry::id); };

    auto prune = [&](auto& ids)
    { ids.erase(std::ranges::remove_if(ids, [&](auto const id) { return !existsInFeed(id); }).begin(), ids.end()); };

    prune(_hiddenCompactIds);
    prune(_hiddenDetailIds);
  }
} // namespace ao::uimodel
