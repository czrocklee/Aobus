// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/NotificationService.h>

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ao::rt
{
  struct NotificationService::Impl final
  {
    NotificationFeedState state;
    std::uint64_t nextId = 0;

    Signal<NotificationId> postedSignal;
    Signal<NotificationId> updatedSignal;
    Signal<NotificationId> dismissedSignal;
    Signal<> changedSignal;
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

  Subscription NotificationService::onUpdated(std::move_only_function<void(NotificationId)> handler)
  {
    return _impl->updatedSignal.connect(std::move(handler));
  }

  Subscription NotificationService::onDismissed(std::move_only_function<void(NotificationId)> handler)
  {
    return _impl->dismissedSignal.connect(std::move(handler));
  }

  Subscription NotificationService::onChanged(std::move_only_function<void()> handler)
  {
    return _impl->changedSignal.connect(std::move(handler));
  }

  NotificationFeedState NotificationService::feed() const
  {
    return _impl->state;
  }

  NotificationId NotificationService::post(NotificationSeverity const severity,
                                           std::string message,
                                           bool const sticky,
                                           std::optional<std::chrono::milliseconds> const optTimeout)
  {
    return post(NotificationRequest{
      .severity = severity,
      .message = std::move(message),
      .sticky = sticky,
      .optTimeout = optTimeout,
    });
  }

  NotificationId NotificationService::post(NotificationRequest request)
  {
    auto const id = NotificationId{++_impl->nextId};

    auto entry = NotificationEntry{
      .id = id,
      .severity = request.severity,
      .message = std::move(request.message),
      .sticky = request.sticky,
      .optTimeout = request.optTimeout,
      .content = std::move(request.content),
    };

    _impl->state.entries.push_back(std::move(entry));
    ++_impl->state.revision;
    _impl->postedSignal.emit(id);
    _impl->changedSignal.emit();

    return id;
  }

  void NotificationService::updateContent(NotificationId const id, NotificationContentState content)
  {
    auto const it = std::ranges::find(_impl->state.entries, id, &NotificationEntry::id);

    if (it != _impl->state.entries.end())
    {
      it->content = std::move(content);
      ++_impl->state.revision;
      _impl->updatedSignal.emit(id);
      _impl->changedSignal.emit();
    }
  }

  void NotificationService::updateProgress(NotificationId const id, NotificationProgressState progress)
  {
    auto const it = std::ranges::find(_impl->state.entries, id, &NotificationEntry::id);

    if (it != _impl->state.entries.end())
    {
      it->content.optProgress = std::move(progress);
      ++_impl->state.revision;
      _impl->updatedSignal.emit(id);
      _impl->changedSignal.emit();
    }
  }

  void NotificationService::clearProgress(NotificationId const id)
  {
    auto const it = std::ranges::find(_impl->state.entries, id, &NotificationEntry::id);

    if (it != _impl->state.entries.end() && it->content.optProgress)
    {
      it->content.optProgress = std::nullopt;
      ++_impl->state.revision;
      _impl->updatedSignal.emit(id);
      _impl->changedSignal.emit();
    }
  }

  void NotificationService::dismiss(NotificationId const id)
  {
    auto const it = std::ranges::find(_impl->state.entries, id, &NotificationEntry::id);

    if (it != _impl->state.entries.end())
    {
      _impl->state.entries.erase(it);
      ++_impl->state.revision;
      _impl->dismissedSignal.emit(id);
      _impl->changedSignal.emit();
    }
  }

  void NotificationService::dismissAll()
  {
    if (!_impl->state.entries.empty())
    {
      _impl->state.entries.clear();
      ++_impl->state.revision;
      _impl->changedSignal.emit();
    }
  }
}
