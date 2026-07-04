// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "NotificationState.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace ao::rt
{
  class NotificationService final
  {
  public:
    NotificationService();
    ~NotificationService();

    NotificationService(NotificationService const&) = delete;
    NotificationService& operator=(NotificationService const&) = delete;
    NotificationService(NotificationService&&) = delete;
    NotificationService& operator=(NotificationService&&) = delete;

    NotificationFeedState feed() const;

    NotificationId post(NotificationSeverity severity,
                        std::string message,
                        bool sticky = false,
                        std::optional<std::chrono::milliseconds> optTimeout = std::nullopt);
    NotificationId post(NotificationRequest request);

    bool updateMessage(NotificationId id, std::string message);
    void updateContent(NotificationId id, NotificationContentState content);
    void updateProgress(NotificationId id, NotificationProgressState progress);
    void clearProgress(NotificationId id);
    void dismiss(NotificationId id);
    void dismissAll();

    Subscription onPosted(std::move_only_function<void(NotificationId)> handler);
    Subscription onUpdated(std::move_only_function<void(NotificationId)> handler);
    Subscription onDismissed(std::move_only_function<void(NotificationId)> handler);
    Subscription onChanged(std::move_only_function<void()> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::rt
