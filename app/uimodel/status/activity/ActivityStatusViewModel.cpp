// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActivityStatusFeedProjection.h"
#include <ao/async/Subscription.h>
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    std::chrono::steady_clock::time_point defaultActivityStatusNow()
    {
      return std::chrono::steady_clock::now();
    }

    bool sameCompactPresentation(ActivityCompactState const& lhs, ActivityCompactState const& rhs)
    {
      return lhs.kind == rhs.kind && lhs.text == rhs.text && lhs.optProgressFraction == rhs.optProgressFraction &&
             lhs.dismissible == rhs.dismissible && lhs.hasDetails == rhs.hasDetails &&
             lhs.optAutoDismissTimeout == rhs.optAutoDismissTimeout;
    }
  } // namespace

  struct ActivityStatusViewModel::Impl final
  {
    rt::NotificationService& notifications;
    std::function<void(ActivityStatusViewState const&)> onRender;
    ActivityStatusClock clock;
    ActivityStatusFeedProjection feedProjection;
    std::optional<std::chrono::steady_clock::time_point> optAutoDismissDeadline{};
    std::optional<ActivityCompactState> optScheduledCompact{};

    async::Subscription feedUpdatedSub;
    async::Subscription libraryProgressSub;
    async::Subscription libraryCompletedSub;

    Impl(rt::NotificationService& notificationService,
         std::function<void(ActivityStatusViewState const&)> renderCallback,
         ActivityStatusViewModelOptions options)
      : notifications{notificationService}, onRender{std::move(renderCallback)}, clock{std::move(options.clock)}
    {
      if (!clock)
      {
        clock = defaultActivityStatusNow;
      }

      auto const initialFeed = notifications.feed();
      feedProjection.initialize(initialFeed);

      feedUpdatedSub = notifications.onFeedUpdated(
        [this](rt::NotificationFeedUpdate const& update)
        {
          if (!update.feedPtr)
          {
            return;
          }

          feedProjection.handleFeedUpdated(update);
          publish();
        });

      if (options.libraryChanges != nullptr)
      {
        libraryProgressSub = options.libraryChanges->onLibraryTaskProgress(
          [this](rt::LibraryChanges::LibraryTaskProgressUpdated const& event) { handleLibraryTaskProgress(event); });
        libraryCompletedSub = options.libraryChanges->onLibraryTaskCompleted(
          [this](rt::LibraryChanges::LibraryTaskCompleted const& event) { handleLibraryTaskCompleted(event); });
      }

      syncAutoDismissDeadline();

      if (options.emitInitialState && onRender)
      {
        onRender(feedProjection.viewState());
      }
    }

    void publish()
    {
      syncAutoDismissDeadline();

      if (onRender)
      {
        onRender(feedProjection.viewState());
      }
    }

    void syncAutoDismissDeadline()
    {
      auto const& compact = feedProjection.viewState().compact;

      if (!compact.optAutoDismissTimeout)
      {
        optAutoDismissDeadline.reset();
        optScheduledCompact.reset();
        return;
      }

      if (optAutoDismissDeadline && optScheduledCompact && sameCompactPresentation(*optScheduledCompact, compact))
      {
        return;
      }

      optScheduledCompact = compact;
      optAutoDismissDeadline = now() + *compact.optAutoDismissTimeout;
    }

    std::chrono::steady_clock::time_point now() const { return clock(); }

    void autoDismissCompact()
    {
      optAutoDismissDeadline.reset();
      feedProjection.autoDismissCompact(notifications.feed());
      publish();
    }

    void dismissCompact()
    {
      feedProjection.dismissCompact(notifications.feed());
      publish();
    }

    void hideDetailNotification(rt::NotificationId const id)
    {
      feedProjection.hideDetailNotification(id, notifications.feed());
      publish();
    }

    void handleLibraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressUpdated const& event)
    {
      feedProjection.handleLibraryTaskProgress(event);
      publish();
    }

    void handleLibraryTaskCompleted(rt::LibraryChanges::LibraryTaskCompleted const& event)
    {
      feedProjection.handleLibraryTaskCompleted(event, notifications.feed());
      publish();
    }
  };

  ActivityStatusViewModel::ActivityStatusViewModel(rt::NotificationService& notifications,
                                                   std::function<void(ActivityStatusViewState const&)> onRender,
                                                   ActivityStatusViewModelOptions options)
    : _implPtr{std::make_unique<Impl>(notifications, std::move(onRender), std::move(options))}
  {
  }

  ActivityStatusViewModel::~ActivityStatusViewModel() = default;

  ActivityStatusViewState const& ActivityStatusViewModel::viewState() const noexcept
  {
    return _implPtr->feedProjection.viewState();
  }

  bool ActivityStatusViewModel::autoDismissCompactIfDue()
  {
    if (!_implPtr->optAutoDismissDeadline || _implPtr->now() < *_implPtr->optAutoDismissDeadline)
    {
      return false;
    }

    autoDismissCompact();
    return true;
  }

  void ActivityStatusViewModel::autoDismissCompact()
  {
    _implPtr->autoDismissCompact();
  }

  void ActivityStatusViewModel::dismissCompact()
  {
    _implPtr->dismissCompact();
  }

  void ActivityStatusViewModel::hideDetailNotification(rt::NotificationId const id)
  {
    _implPtr->hideDetailNotification(id);
  }
} // namespace ao::uimodel
