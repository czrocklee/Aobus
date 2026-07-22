// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "NotificationIds.h"
#include <ao/CoreIds.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::rt
{
  enum class NotificationSeverity : std::uint8_t
  {
    Info,
    Warning,
    Error,
  };

  enum class NotificationReportTemplate : std::uint8_t
  {
    PlaybackTrackOpenFailed,
    PlaybackDecodeFailed,
    PlaybackRouteActivationFailed,
    PlaybackDeviceLost,
    PlaybackSequenceFinished,
    PlaybackTracksSkipped,
    PlaybackStoppedAfterFailures,
    PlaybackStoppedForTrack,
  };

  struct NotificationReport final
  {
    NotificationReportTemplate templateId = NotificationReportTemplate::PlaybackSequenceFinished;
    TrackId trackId = kInvalidTrackId;
    std::string subject{};
    std::string detail{};
    std::size_t count = 0;

    bool operator==(NotificationReport const&) const = default;
  };

  using NotificationMessage = std::variant<std::string, NotificationReport>;

  enum class NotificationLifetimeKind : std::uint8_t
  {
    Transient,
    History,
    Pinned,
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

    static constexpr NotificationLifetime history() noexcept
    {
      return NotificationLifetime{NotificationLifetimeKind::History, {}};
    }

    static constexpr NotificationLifetime pinned() noexcept
    {
      return NotificationLifetime{NotificationLifetimeKind::Pinned, {}};
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

    NotificationLifetimeKind _kind = NotificationLifetimeKind::History;
    std::chrono::milliseconds _duration{};
  };

  struct NotificationRequest final
  {
    NotificationSeverity severity = NotificationSeverity::Info;
    NotificationMessage message{};
    NotificationLifetime lifetime;
  };

  struct NotificationEntry final
  {
    NotificationId id{};
    std::optional<NotificationReportKey> optReportKey{};
    NotificationSeverity severity = NotificationSeverity::Info;
    NotificationMessage message{};
    NotificationLifetime lifetime = NotificationLifetime::history();

    bool operator==(NotificationEntry const&) const = default;
  };

  struct NotificationFeedState final
  {
    std::vector<NotificationEntry> entries{};
  };

  enum class NotificationFeedMutationKind : std::uint8_t
  {
    Posted,
    ReportUpdated,
    Expired,
  };

  struct NotificationFeedLimits final
  {
    static constexpr std::size_t kDefaultMaxEntries = 256;
    static constexpr std::size_t kDefaultMaxHistoryEntries = 128;
    static constexpr std::size_t kDefaultMaxTextBytes = 4096;
    static constexpr std::size_t kDefaultMaxTotalTextBytes = std::size_t{256} * 1024;

    std::size_t maxEntries = kDefaultMaxEntries;
    std::size_t maxHistoryEntries = kDefaultMaxHistoryEntries;
    std::size_t maxTextBytes = kDefaultMaxTextBytes;
    std::size_t maxTotalTextBytes = kDefaultMaxTotalTextBytes;
  };

  struct NotificationFeedUpdate final
  {
    NotificationFeedMutationKind mutationKind = NotificationFeedMutationKind::Posted;
    NotificationId id{};
    // Service-produced updates always carry a non-null immutable snapshot.
    std::shared_ptr<NotificationFeedState const> feedPtr{};
  };
} // namespace ao::rt
