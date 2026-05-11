// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "NotificationService.h"
#include <algorithm>
#include <ranges>

namespace ao::rt
{
  struct NotificationService::Impl final
  {
    NotificationFeedState state;
    std::uint64_t nextId = 0;

    Signal<NotificationId> postedSignal;
    Signal<NotificationId> dismissedSignal;
  };

  NotificationService::NotificationService()
    : _impl{std::make_unique<Impl>()}
  {
  }

  NotificationService::~NotificationService() = default;

  Subscription NotificationService::onPosted(std::move_only_function<void(NotificationId)> handler)
  {
    return _impl->postedSignal.connect(std::move(handler));
  }

  Subscription NotificationService::onDismissed(std::move_only_function<void(NotificationId)> handler)
  {
    return _impl->dismissedSignal.connect(std::move(handler));
  }

  NotificationFeedState NotificationService::feed() const
  {
    return _impl->state;
  }

  NotificationId NotificationService::post(NotificationSeverity severity,
                                           std::string message,
                                           bool sticky,
                                           std::optional<std::chrono::milliseconds> optTimeout)
  {
    auto const id = NotificationId{++_impl->nextId};

    auto entry = NotificationEntry{
      .id = id,
      .severity = severity,
      .message = std::move(message),
      .sticky = sticky,
      .optTimeout = optTimeout,
    };

    _impl->state.entries.push_back(std::move(entry));
    _impl->postedSignal.emit(id);

    return id;
  }

  void NotificationService::dismiss(NotificationId id)
  {
    auto const it = std::ranges::find(_impl->state.entries, id, &NotificationEntry::id);

    if (it != _impl->state.entries.end())
    {
      _impl->state.entries.erase(it);
      _impl->dismissedSignal.emit(id);
    }
  }

  void NotificationService::dismissAll()
  {
    _impl->state.entries.clear();
  }
}
