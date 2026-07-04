// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>

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
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  NotificationService::~NotificationService() = default;

  Subscription NotificationService::onPosted(std::move_only_function<void(NotificationId)> handler)
  {
    return _implPtr->postedSignal.connect(std::move(handler));
  }

  Subscription NotificationService::onUpdated(std::move_only_function<void(NotificationId)> handler)
  {
    return _implPtr->updatedSignal.connect(std::move(handler));
  }

  Subscription NotificationService::onDismissed(std::move_only_function<void(NotificationId)> handler)
  {
    return _implPtr->dismissedSignal.connect(std::move(handler));
  }

  Subscription NotificationService::onChanged(std::move_only_function<void()> handler)
  {
    return _implPtr->changedSignal.connect(std::move(handler));
  }

  NotificationFeedState NotificationService::feed() const
  {
    return _implPtr->state;
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
    auto const id = NotificationId{++_implPtr->nextId};

    auto entry = NotificationEntry{
      .id = id,
      .severity = request.severity,
      .message = std::move(request.message),
      .sticky = request.sticky,
      .optTimeout = request.optTimeout,
      .activityPresentation = request.activityPresentation,
      .content = std::move(request.content),
    };

    _implPtr->state.entries.push_back(std::move(entry));
    ++_implPtr->state.revision;
    _implPtr->postedSignal.emit(id);
    _implPtr->changedSignal.emit();

    return id;
  }

  bool NotificationService::updateMessage(NotificationId const id, std::string message)
  {
    auto const it = std::ranges::find(_implPtr->state.entries, id, &NotificationEntry::id);

    if (it != _implPtr->state.entries.end())
    {
      it->message = std::move(message);
      ++_implPtr->state.revision;
      _implPtr->updatedSignal.emit(id);
      _implPtr->changedSignal.emit();
      return true;
    }

    return false;
  }

  void NotificationService::updateContent(NotificationId const id, NotificationContentState content)
  {
    auto const it = std::ranges::find(_implPtr->state.entries, id, &NotificationEntry::id);

    if (it != _implPtr->state.entries.end())
    {
      it->content = std::move(content);
      ++_implPtr->state.revision;
      _implPtr->updatedSignal.emit(id);
      _implPtr->changedSignal.emit();
    }
  }

  void NotificationService::updateProgress(NotificationId const id, NotificationProgressState progress)
  {
    auto const it = std::ranges::find(_implPtr->state.entries, id, &NotificationEntry::id);

    if (it != _implPtr->state.entries.end())
    {
      it->content.optProgress = std::move(progress);
      ++_implPtr->state.revision;
      _implPtr->updatedSignal.emit(id);
      _implPtr->changedSignal.emit();
    }
  }

  void NotificationService::clearProgress(NotificationId const id)
  {
    auto const it = std::ranges::find(_implPtr->state.entries, id, &NotificationEntry::id);

    if (it != _implPtr->state.entries.end() && it->content.optProgress)
    {
      it->content.optProgress = std::nullopt;
      ++_implPtr->state.revision;
      _implPtr->updatedSignal.emit(id);
      _implPtr->changedSignal.emit();
    }
  }

  void NotificationService::dismiss(NotificationId const id)
  {
    auto const it = std::ranges::find(_implPtr->state.entries, id, &NotificationEntry::id);

    if (it != _implPtr->state.entries.end())
    {
      _implPtr->state.entries.erase(it);
      ++_implPtr->state.revision;
      _implPtr->dismissedSignal.emit(id);
      _implPtr->changedSignal.emit();
    }
  }

  void NotificationService::dismissAll()
  {
    if (!_implPtr->state.entries.empty())
    {
      _implPtr->state.entries.clear();
      ++_implPtr->state.revision;
      _implPtr->changedSignal.emit();
    }
  }
} // namespace ao::rt
