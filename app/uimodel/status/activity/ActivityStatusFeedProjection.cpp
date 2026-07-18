// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActivityStatusFeedProjection.h"

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/library/track/TrackCountFormatter.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
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

    bool isDetailPresented(rt::NotificationEntry const& entry)
    {
      return entry.activityPresentation != rt::NotificationActivityPresentation::Hidden;
    }

    bool isCompactPresented(rt::NotificationEntry const& entry)
    {
      return entry.activityPresentation == rt::NotificationActivityPresentation::Default;
    }

    bool hasRichDetail(rt::NotificationEntry const& entry)
    {
      return entry.sticky || entry.content.optProgress || !entry.content.title.empty() ||
             !entry.content.iconName.empty() || !entry.content.actions.empty();
    }

    bool isDetailEligible(rt::NotificationEntry const& entry)
    {
      if (!isDetailPresented(entry))
      {
        return false;
      }

      if (entry.activityPresentation == rt::NotificationActivityPresentation::DetailOnly)
      {
        return true;
      }

      return isPersistentSeverity(entry.severity) || hasRichDetail(entry);
    }

    bool isDetailClearable(rt::NotificationEntry const& entry)
    {
      return isDetailEligible(entry) && !entry.sticky && !entry.content.optProgress;
    }

    std::string compactProgressText(std::string message)
    {
      if (message.starts_with("Scanning:"))
      {
        return "Scanning library";
      }

      if (message.starts_with("Updating:"))
      {
        return "Updating library";
      }

      return message;
    }

    std::string primaryText(rt::NotificationEntry const& entry)
    {
      return entry.content.title.empty() ? entry.message : entry.content.title;
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

    ActivityDetailItem detailItem(rt::NotificationEntry const& entry)
    {
      auto item = ActivityDetailItem{
        .id = entry.id,
        .severity = entry.severity,
        .title = entry.content.title,
        .message = entry.message,
        .iconName = entry.content.iconName,
        .sticky = entry.sticky,
        .dismissible = isDetailClearable(entry),
      };

      if (entry.content.optProgress)
      {
        item.optProgressMode = entry.content.optProgress->mode;
        item.progressFraction = entry.content.optProgress->fraction;
        item.progressLabel = entry.content.optProgress->label;
      }

      item.actions.reserve(entry.content.actions.size());

      for (auto const& action : entry.content.actions)
      {
        item.actions.push_back(ActivityActionDescriptor{.id = action.id, .label = action.label});
      }

      return item;
    }
  } // namespace

  bool hasDetailContent(ActivityDetailState const& detail) noexcept
  {
    return !detail.items.empty() || detail.optLibraryTask;
  }

  std::vector<ActivityResolvedActionState> resolveActivityActionStates(
    std::vector<ActivityActionDescriptor> const& actions,
    ActivityActionAvailabilityResolver const& resolveAction,
    std::size_t const maxVisibleActions)
  {
    auto result = std::vector<ActivityResolvedActionState>{};

    if (!resolveAction || maxVisibleActions == 0)
    {
      return result;
    }

    result.reserve(std::min(actions.size(), maxVisibleActions));

    for (auto const& action : actions)
    {
      if (result.size() >= maxVisibleActions)
      {
        break;
      }

      auto state = resolveAction(action.id, action.label);

      if (!state.visible || state.label.empty())
      {
        continue;
      }

      result.push_back(ActivityResolvedActionState{.id = action.id,
                                                   .enabled = state.enabled,
                                                   .label = std::move(state.label),
                                                   .disabledReason = std::move(state.disabledReason)});
    }

    return result;
  }

  void ActivityStatusFeedProjection::initialize(rt::NotificationFeedState const& feed)
  {
    projectDetail(feed);
    projectPersistentCompact(feed);
  }

  void ActivityStatusFeedProjection::handleFeedUpdated(rt::NotificationFeedUpdate const& update)
  {
    if (!update.feedPtr || update.revision != update.feedPtr->revision)
    {
      return;
    }

    auto const& feed = *update.feedPtr;

    if (update.mutationKind == rt::NotificationFeedMutationKind::Posted && update.affectedIds.size() == 1)
    {
      handleNotificationPosted(feed, update.affectedIds.front());
      return;
    }

    if (refreshesVisibleTransient(update))
    {
      auto const sourceId = _state.compact.sourceNotificationIds.front();
      auto const iter = std::ranges::find(feed.entries, sourceId, &rt::NotificationEntry::id);

      if (iter != feed.entries.end() && isCompactPresented(*iter))
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
        persistentKind && hasPresentedCompactSource(_state.compact, feed) && !isCompactSourceDismissed(_state.compact))
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
      projectPersistentCompact(feed);

      bool const sourceStillValid =
        previousCompact.sourceNotificationIds.empty() ||
        (hasPresentedCompactSource(previousCompact, feed) && !isCompactSourceDismissed(previousCompact));

      if (_state.compact.kind == ActivityStatusKind::Idle && sourceStillValid)
      {
        _state.compact = std::move(previousCompact);
      }
    }
  }

  void ActivityStatusFeedProjection::handleNotificationPosted(rt::NotificationFeedState const& feed,
                                                              rt::NotificationId const id)
  {
    projectDetail(feed);

    auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);

    if (iter == feed.entries.end() || !isCompactPresented(*iter))
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
    if (_taskActive || _state.compact.kind != ActivityStatusKind::Info ||
        _state.compact.sourceNotificationIds.size() != 1)
    {
      return false;
    }

    switch (update.mutationKind)
    {
      case rt::NotificationFeedMutationKind::MessageUpdated:
      case rt::NotificationFeedMutationKind::ContentUpdated:
      case rt::NotificationFeedMutationKind::ProgressUpdated:
      case rt::NotificationFeedMutationKind::ProgressCleared:
        return std::ranges::contains(update.affectedIds, _state.compact.sourceNotificationIds.front());
      case rt::NotificationFeedMutationKind::Posted:
      case rt::NotificationFeedMutationKind::Dismissed:
      case rt::NotificationFeedMutationKind::Cleared: return false;
    }

    return false;
  }

  void ActivityStatusFeedProjection::handleLibraryTaskProgress(std::string message, double const fraction)
  {
    _taskActive = true;
    _optLibraryProgress = LibraryProgressState{.message = std::move(message), .fraction = fraction};
    _state.compact = ActivityCompactState{
      .kind = ActivityStatusKind::Processing,
      .text = compactProgressText(_optLibraryProgress->message),
      .optProgressFraction = fraction,
    };
    _state.detail.optLibraryTask =
      ActivityTaskDetail{.message = _optLibraryProgress->message, .progressFraction = fraction};
    _state.detail.hasActiveProgress = true;
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
    rememberDismissedCompactSources();
    _state.compact = ActivityCompactState{};
    projectDetail(feed);
  }

  void ActivityStatusFeedProjection::dismissDetailNotificationFromActivity(rt::NotificationId const id,
                                                                           rt::NotificationFeedState const& feed)
  {
    auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);

    if (iter == feed.entries.end() || !isDetailClearable(*iter))
    {
      return;
    }

    if (!std::ranges::contains(_detailDismissedNotificationIds, id))
    {
      _detailDismissedNotificationIds.push_back(id);
    }

    bool const compactReferencedDismissedSource = std::ranges::contains(_state.compact.sourceNotificationIds, id);
    projectDetail(feed);

    if (compactReferencedDismissedSource && !_taskActive)
    {
      projectPersistentCompact(feed);
    }
  }

  void ActivityStatusFeedProjection::handleTransientExpired(rt::NotificationFeedState const& feed)
  {
    projectDetail(feed);
    projectPersistentCompact(feed);
  }

  std::vector<rt::NotificationId> ActivityStatusFeedProjection::locallyHideableNotificationIds(
    rt::NotificationFeedState const& feed) const
  {
    return feed.entries | std::views::filter(isDetailEligible) |
           std::views::filter([this](auto const& entry)
                              { return !std::ranges::contains(_detailDismissedNotificationIds, entry.id); }) |
           std::views::filter([](auto const& entry) { return !entry.sticky && !entry.content.optProgress; }) |
           std::views::transform(&rt::NotificationEntry::id) | std::ranges::to<std::vector>();
  }

  ActivityStatusViewState const& ActivityStatusFeedProjection::viewState() const noexcept
  {
    return _state;
  }

  void ActivityStatusFeedProjection::projectDetail(rt::NotificationFeedState const& feed)
  {
    pruneDismissedSources(feed);

    auto items = std::vector<ActivityDetailItem>{};
    items.reserve(feed.entries.size());
    bool hasActiveProgress = _optLibraryProgress.has_value();

    for (auto const& entry : feed.entries)
    {
      if (!isDetailEligible(entry))
      {
        continue;
      }

      if (std::ranges::contains(_detailDismissedNotificationIds, entry.id))
      {
        continue;
      }

      if (entry.content.optProgress)
      {
        hasActiveProgress = true;
      }

      items.push_back(detailItem(entry));
    }

    std::ranges::sort(
      items,
      [](auto const& lhs, auto const& rhs)
      {
        if (bool const lhsProgress = lhs.optProgressMode.has_value(), rhsProgress = rhs.optProgressMode.has_value();
            lhsProgress != rhsProgress)
        {
          return lhsProgress;
        }

        return lhs.id > rhs.id;
      });

    auto optLibraryTask = std::optional<ActivityTaskDetail>{};

    if (_optLibraryProgress)
    {
      optLibraryTask =
        ActivityTaskDetail{.message = _optLibraryProgress->message, .progressFraction = _optLibraryProgress->fraction};
    }

    _state.detail = ActivityDetailState{
      .items = std::move(items), .optLibraryTask = std::move(optLibraryTask), .hasActiveProgress = hasActiveProgress};
  }

  void ActivityStatusFeedProjection::projectPersistentCompact(rt::NotificationFeedState const& feed)
  {
    auto optSeverity = std::optional<rt::NotificationSeverity>{};
    auto ids = std::vector<rt::NotificationId>{};
    rt::NotificationEntry const* latestEntry = nullptr;

    for (auto const& entry : feed.entries)
    {
      if (!isCompactPresented(entry))
      {
        continue;
      }

      if (!isPersistentSeverity(entry.severity))
      {
        continue;
      }

      if (isCompactSourceSuppressed(entry.id))
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
      _state.compact = ActivityCompactState{};
      return;
    }

    auto text = groupedText(*optSeverity, ids.size());

    if (text.empty())
    {
      text = primaryText(*latestEntry);
    }

    _state.compact = ActivityCompactState{
      .kind = kindForSeverity(*optSeverity),
      .text = std::move(text),
      .groupedCount = ids.size(),
      .persistent = true,
      .dismissible = true,
      .hasDetails = !ids.empty(),
      .sourceNotificationIds = std::move(ids),
    };
  }

  void ActivityStatusFeedProjection::projectNotificationCompact(rt::NotificationEntry const& entry)
  {
    _state.compact = ActivityCompactState{
      .kind = kindForSeverity(entry.severity),
      .text = primaryText(entry),
      .groupedCount = 1,
      .persistent = isPersistentSeverity(entry.severity),
      .dismissible = true,
      .hasDetails = isDetailEligible(entry),
      .optAutoDismissTimeout = isPersistentSeverity(entry.severity)
                                 ? std::optional<std::chrono::milliseconds>{}
                                 : entry.optTimeout.value_or(kActivityStatusDefaultAutoDismissTimeout),
      .sourceNotificationIds = {entry.id},
    };
  }

  void ActivityStatusFeedProjection::projectCompletionCompact(std::size_t const count)
  {
    _state.compact = ActivityCompactState{
      .kind = ActivityStatusKind::Success,
      .text = count == 0 ? std::string{"Library is up to date"}
                         : std::format("Scan complete: {} added", formatTrackCount(count)),
      .optAutoDismissTimeout = kActivityStatusDefaultAutoDismissTimeout,
    };
  }

  bool ActivityStatusFeedProjection::hasPresentedCompactSource(ActivityCompactState const& compact,
                                                               rt::NotificationFeedState const& feed) const
  {
    return std::ranges::any_of(compact.sourceNotificationIds,
                               [&feed](auto const id)
                               {
                                 auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);
                                 return iter != feed.entries.end() && isCompactPresented(*iter);
                               });
  }

  bool ActivityStatusFeedProjection::isCompactSourceDismissed(ActivityCompactState const& compact) const
  {
    return !compact.sourceNotificationIds.empty() &&
           std::ranges::all_of(
             compact.sourceNotificationIds, [this](auto const id) { return isCompactSourceSuppressed(id); });
  }

  bool ActivityStatusFeedProjection::isCompactSourceSuppressed(rt::NotificationId const id) const
  {
    return std::ranges::contains(_compactDismissedNotificationIds, id) ||
           std::ranges::contains(_detailDismissedNotificationIds, id);
  }

  void ActivityStatusFeedProjection::rememberDismissedCompactSources()
  {
    for (auto const id : _state.compact.sourceNotificationIds)
    {
      if (!std::ranges::contains(_compactDismissedNotificationIds, id))
      {
        _compactDismissedNotificationIds.push_back(id);
      }
    }
  }

  void ActivityStatusFeedProjection::pruneDismissedSources(rt::NotificationFeedState const& feed)
  {
    auto const existsInFeed = [&feed](auto const id)
    { return std::ranges::contains(feed.entries, id, &rt::NotificationEntry::id); };

    auto prune = [&](auto& ids)
    { ids.erase(std::ranges::remove_if(ids, [&](auto const id) { return !existsInFeed(id); }).begin(), ids.end()); };

    prune(_compactDismissedNotificationIds);
    prune(_detailDismissedNotificationIds);
  }
} // namespace ao::uimodel
