// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "CorePrimitives.h"
#include "StateTypes.h"
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace ao::app
{
  class EventBus;

  class NotificationService final
  {
  public:
    NotificationService(EventBus& events);
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

    void dismiss(NotificationId id);
    void dismissAll();

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
