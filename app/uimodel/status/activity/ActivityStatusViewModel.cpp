// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ActivityStatusFeedState.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <chrono>
#include <cstddef>
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
    ActivityStatusFeedState feedState;
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

      feedState.initialize(notifications.feed());

      postedSub = notifications.onPosted(
        [this](rt::NotificationId const id)
        {
          feedState.onNotificationPosted(notifications.feed(), id);
          publish();
        });
      changedSub = notifications.onChanged(
        [this]
        {
          feedState.onFeedChanged(notifications.feed());
          publish();
        });

      if (options.libraryChanges != nullptr)
      {
        libraryProgressSub = options.libraryChanges->onLibraryTaskProgress(
          [this](rt::LibraryChanges::LibraryTaskProgressUpdated const& event)
          { onLibraryTaskProgress(event.message, event.fraction); });
        libraryCompletedSub = options.libraryChanges->onLibraryTaskCompleted([this](std::size_t const count)
                                                                             { onLibraryTaskCompleted(count); });
      }

      syncAutoDismissDeadline();

      if (options.emitInitialState && onRender)
      {
        onRender(feedState.viewState());
      }
    }

    void publish()
    {
      syncAutoDismissDeadline();

      if (onRender)
      {
        onRender(feedState.viewState());
      }
    }

    void syncAutoDismissDeadline()
    {
      auto const& compact = feedState.viewState().compact;

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
      feedState.onTransientExpired(notifications.feed());
      publish();
    }

    void dismissCompact()
    {
      feedState.dismissCompact(notifications.feed());
      publish();
    }

    void hideDetailNotificationFromActivity(rt::NotificationId const id)
    {
      feedState.hideDetailNotificationFromActivity(id, notifications.feed());
      publish();
    }

    void onLibraryTaskProgress(std::string message, double const fraction)
    {
      feedState.onLibraryTaskProgress(std::move(message), fraction);
      publish();
    }

    void onLibraryTaskCompleted(std::size_t const count)
    {
      feedState.onLibraryTaskCompleted(count, notifications.feed());
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
    return _implPtr->feedState.viewState();
  }

  bool ActivityStatusViewModel::hasPendingAutoDismiss() const noexcept
  {
    return static_cast<bool>(_implPtr->optAutoDismissDeadline);
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

  void ActivityStatusViewModel::hideDetailNotificationFromActivity(rt::NotificationId const id)
  {
    _implPtr->hideDetailNotificationFromActivity(id);
  }

  void ActivityStatusViewModel::onLibraryTaskProgress(std::string message, double const fraction)
  {
    _implPtr->onLibraryTaskProgress(std::move(message), fraction);
  }

  void ActivityStatusViewModel::onLibraryTaskCompleted(std::size_t const count)
  {
    _implPtr->onLibraryTaskCompleted(count);
  }
} // namespace ao::uimodel
