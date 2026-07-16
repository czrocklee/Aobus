// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActivityStatusFeedProjection.h"
#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/Subscription.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    std::chrono::steady_clock::time_point defaultActivityStatusNow()
    {
      return std::chrono::steady_clock::now();
    }
  } // namespace

  struct ActivityStatusViewModel::Impl final
  {
    rt::NotificationService& notifications;
    std::function<void(ActivityStatusViewState const&)> onRender;
    ActivityStatusClock clock;
    ActivityStatusFeedProjection feedProjection;
    std::optional<std::chrono::steady_clock::time_point> optAutoDismissDeadline{};

    rt::Subscription postedSub;
    rt::Subscription changedSub;
    rt::Subscription libraryProgressSub;
    rt::Subscription libraryCompletedSub;

    Impl(rt::NotificationService& notificationService,
         std::function<void(ActivityStatusViewState const&)> renderCallback,
         ActivityStatusViewModelOptions options)
      : notifications{notificationService}, onRender{std::move(renderCallback)}, clock{std::move(options.clock)}
    {
      if (!clock)
      {
        clock = defaultActivityStatusNow;
      }

      feedProjection.initialize(notifications.feed());

      postedSub = notifications.onPosted(
        [this](rt::NotificationId const id)
        {
          feedProjection.handleNotificationPosted(notifications.feed(), id);
          publish();
        });
      changedSub = notifications.onChanged(
        [this]
        {
          feedProjection.handleFeedChanged(notifications.feed());
          publish();
        });

      if (options.libraryChanges != nullptr)
      {
        libraryProgressSub = options.libraryChanges->onLibraryTaskProgress(
          [this](rt::LibraryChanges::LibraryTaskProgressUpdated const& event)
          { handleLibraryTaskProgress(event.message, event.fraction); });
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
        return;
      }

      optAutoDismissDeadline = now() + *compact.optAutoDismissTimeout;
    }

    std::chrono::steady_clock::time_point now() const { return clock(); }

    void expireTransient()
    {
      optAutoDismissDeadline.reset();
      feedProjection.handleTransientExpired(notifications.feed());
      publish();
    }

    void dismissCompact()
    {
      feedProjection.dismissCompact(notifications.feed());
      publish();
    }

    void dismissDetailNotificationFromActivity(rt::NotificationId const id)
    {
      feedProjection.dismissDetailNotificationFromActivity(id, notifications.feed());
      publish();
    }

    void handleLibraryTaskProgress(std::string message, double const fraction)
    {
      feedProjection.handleLibraryTaskProgress(std::move(message), fraction);
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

  bool ActivityStatusViewModel::hasPendingAutoDismiss() const noexcept
  {
    return _implPtr->optAutoDismissDeadline.has_value();
  }

  bool ActivityStatusViewModel::expireTransientIfDue()
  {
    if (!_implPtr->optAutoDismissDeadline || _implPtr->now() < *_implPtr->optAutoDismissDeadline)
    {
      return false;
    }

    expireTransient();
    return true;
  }

  void ActivityStatusViewModel::expireTransient()
  {
    _implPtr->expireTransient();
  }

  void ActivityStatusViewModel::dismissCompact()
  {
    _implPtr->dismissCompact();
  }

  void ActivityStatusViewModel::dismissDetailNotificationFromActivity(rt::NotificationId const id)
  {
    _implPtr->dismissDetailNotificationFromActivity(id);
  }

  void ActivityStatusViewModel::handleLibraryTaskProgress(std::string message, double const fraction)
  {
    _implPtr->handleLibraryTaskProgress(std::move(message), fraction);
  }

  void ActivityStatusViewModel::handleLibraryTaskCompleted(rt::LibraryChanges::LibraryTaskCompleted const& event)
  {
    _implPtr->handleLibraryTaskCompleted(event);
  }
} // namespace ao::uimodel
