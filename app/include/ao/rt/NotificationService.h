// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "NotificationIds.h"
#include "NotificationState.h"
#include "Subscription.h"

#include <functional>
#include <memory>
#include <string>

namespace ao::async
{
  class Runtime;
}

namespace ao::rt
{
  class NotificationService final
  {
  public:
    // The runtime must outlive this service. Construction, every member call,
    // and subscription teardown belong to its callback executor. Effective
    // commands synchronously publish one immutable update after commit;
    // transient expiry returns through that same executor.
    explicit NotificationService(async::Runtime& runtime);
    ~NotificationService();

    NotificationService(NotificationService const&) = delete;
    NotificationService& operator=(NotificationService const&) = delete;
    NotificationService(NotificationService&&) = delete;
    NotificationService& operator=(NotificationService&&) = delete;

    NotificationFeedState feed() const;

    NotificationId post(NotificationSeverity severity, std::string message, NotificationLifetime lifetime);
    NotificationId post(NotificationRequest request);

    bool updateMessage(NotificationId id, std::string message);
    void updateContent(NotificationId id, NotificationContentState content);
    void updateProgress(NotificationId id, NotificationProgressState progress);
    void clearProgress(NotificationId id);
    void dismiss(NotificationId id);
    void dismissAll();

    // The update reference is callback-scoped; copy feedPtr to retain the
    // immutable revision snapshot beyond the callback.
    Subscription onFeedUpdated(std::move_only_function<void(NotificationFeedUpdate const&)> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
