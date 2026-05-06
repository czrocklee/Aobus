// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "NotificationService.h"
#include "EventBus.h"
#include "EventTypes.h"

#include <algorithm>
#include <ranges>

namespace ao::app
{
  struct NotificationService::Impl final
  {
    EventBus& events;
    NotificationFeedState state;
    std::uint64_t nextId = 0;
  };

  NotificationService::NotificationService(EventBus& events)
    : _impl{std::make_unique<Impl>(events)}
  {
  }

  NotificationService::~NotificationService() = default;

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
    _impl->events.publish(NotificationPosted{.id = id});

    return id;
  }

  void NotificationService::dismiss(NotificationId id)
  {
    auto const it = std::ranges::find(_impl->state.entries, id, &NotificationEntry::id);

    if (it != _impl->state.entries.end())
    {
      _impl->state.entries.erase(it);
      _impl->events.publish(NotificationDismissed{.id = id});
    }
  }

  void NotificationService::dismissAll()
  {
    _impl->state.entries.clear();
  }
}
