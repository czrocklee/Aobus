// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "NotificationIds.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  enum class NotificationSeverity : std::uint8_t
  {
    Info,
    Warning,
    Error,
  };

  enum class NotificationTopic : std::uint8_t
  {
    General,
    PlaybackSequence,
    PlaybackError,
  };

  inline constexpr std::string_view kDefaultNotificationTemplate = "notification.message";

  enum class NotificationProgressMode : std::uint8_t
  {
    Indeterminate,
    Fraction,
  };

  struct NotificationProgressState final
  {
    NotificationProgressMode mode = NotificationProgressMode::Indeterminate;
    double fraction = 0.0;
    std::string label{};
  };

  struct NotificationAction final
  {
    std::string id{};
    std::string label{};
  };

  struct NotificationContentState final
  {
    NotificationTopic topic = NotificationTopic::General;
    std::string templateId = std::string{kDefaultNotificationTemplate};
    std::string title{};
    std::string iconName{};
    std::vector<NotificationAction> actions{};
    std::optional<NotificationProgressState> optProgress{};
  };

  enum class NotificationActivityPresentation : std::uint8_t
  {
    Default,
    DetailOnly,
    Hidden,
  };

  enum class NotificationLifetimeKind : std::uint8_t
  {
    Transient,
    SessionHistory,
    UntilDismissed,
  };

  inline constexpr std::chrono::milliseconds kDefaultNotificationTransientDuration{5000};

  class NotificationLifetime final
  {
  public:
    static constexpr NotificationLifetime transient(
      std::chrono::milliseconds duration = kDefaultNotificationTransientDuration) noexcept
    {
      return NotificationLifetime{NotificationLifetimeKind::Transient, duration};
    }

    static constexpr NotificationLifetime sessionHistory() noexcept
    {
      return NotificationLifetime{NotificationLifetimeKind::SessionHistory, {}};
    }

    static constexpr NotificationLifetime untilDismissed() noexcept
    {
      return NotificationLifetime{NotificationLifetimeKind::UntilDismissed, {}};
    }

    constexpr NotificationLifetimeKind kind() const noexcept { return _kind; }

    constexpr std::optional<std::chrono::milliseconds> optTransientDuration() const noexcept
    {
      if (_kind == NotificationLifetimeKind::Transient)
      {
        return _duration;
      }

      return std::nullopt;
    }

    friend constexpr bool operator==(NotificationLifetime const&, NotificationLifetime const&) = default;

  private:
    constexpr NotificationLifetime(NotificationLifetimeKind const kind,
                                   std::chrono::milliseconds const duration) noexcept
      : _kind{kind}, _duration{duration}
    {
    }

    NotificationLifetimeKind _kind = NotificationLifetimeKind::SessionHistory;
    std::chrono::milliseconds _duration{};
  };

  struct NotificationRequest final
  {
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    NotificationLifetime lifetime;
    NotificationActivityPresentation activityPresentation = NotificationActivityPresentation::Default;
    NotificationContentState content{};
  };

  struct NotificationEntry final
  {
    NotificationId id{};
    NotificationSeverity severity = NotificationSeverity::Info;
    std::string message{};
    NotificationLifetime lifetime = NotificationLifetime::sessionHistory();
    std::uint64_t lifetimeGeneration = 0;
    NotificationActivityPresentation activityPresentation = NotificationActivityPresentation::Default;
    NotificationContentState content{};
  };

  struct NotificationFeedState final
  {
    std::vector<NotificationEntry> entries{};
    std::uint64_t revision = 0;
  };

  enum class NotificationFeedMutationKind : std::uint8_t
  {
    Posted,
    MessageUpdated,
    ContentUpdated,
    ProgressUpdated,
    ProgressCleared,
    Expired,
    Dismissed,
    Cleared,
  };

  struct NotificationFeedUpdate final
  {
    std::uint64_t revision = 0;
    NotificationFeedMutationKind mutationKind = NotificationFeedMutationKind::Posted;
    std::vector<NotificationId> affectedIds{};
    // Service-produced updates always carry a non-null immutable snapshot
    // whose revision equals the update revision.
    std::shared_ptr<NotificationFeedState const> feedPtr{};
  };
} // namespace ao::rt
