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
#include <string_view>
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

  enum class NotificationTopic : std::uint8_t
  {
    General,
    PlaybackSequence,
    PlaybackError,
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

  inline std::string_view resolvedNotificationText(NotificationMessage const& message) noexcept
  {
    if (auto const* text = std::get_if<std::string>(&message); text != nullptr)
    {
      return *text;
    }

    return {};
  }

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

    bool operator==(NotificationProgressState const&) const = default;
  };

  struct NotificationAction final
  {
    std::string id{};
    std::string label{};

    bool operator==(NotificationAction const&) const = default;
  };

  struct NotificationContentState final
  {
    NotificationTopic topic = NotificationTopic::General;
    std::string title{};
    std::string iconName{};
    std::vector<NotificationAction> actions{};
    std::optional<NotificationProgressState> optProgress{};

    bool operator==(NotificationContentState const&) const = default;
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
    NotificationMessage message{};
    NotificationLifetime lifetime;
    NotificationActivityPresentation activityPresentation = NotificationActivityPresentation::Default;
    NotificationContentState content{};
  };

  struct NotificationEntry final
  {
    NotificationId id{};
    std::optional<NotificationReportKey> optReportKey{};
    NotificationSeverity severity = NotificationSeverity::Info;
    NotificationMessage message{};
    NotificationLifetime lifetime = NotificationLifetime::sessionHistory();
    std::uint64_t lifetimeGeneration = 0;
    NotificationActivityPresentation activityPresentation = NotificationActivityPresentation::Default;
    NotificationContentState content{};

    bool operator==(NotificationEntry const&) const = default;
  };

  struct NotificationFeedState final
  {
    std::vector<NotificationEntry> entries{};
    std::uint64_t revision = 0;
  };

  enum class NotificationFeedMutationKind : std::uint8_t
  {
    Posted,
    ReportUpdated,
    MessageUpdated,
    ContentUpdated,
    ProgressUpdated,
    ProgressCleared,
    Expired,
    Dismissed,
    Cleared,
  };

  enum class NotificationMutationOutcome : std::uint8_t
  {
    Applied,
    Missing,
    Unchanged,
    Rejected,
  };

  struct NotificationMutationReply final
  {
    NotificationMutationOutcome outcome = NotificationMutationOutcome::Rejected;
    NotificationId id = kInvalidNotificationId;

    bool operator==(NotificationMutationReply const&) const = default;
  };

  struct NotificationFeedLimits final
  {
    static constexpr std::size_t kDefaultMaxEntries = 256;
    static constexpr std::size_t kDefaultMaxSessionHistoryEntries = 128;
    static constexpr std::size_t kDefaultMaxActionsPerEntry = 8;
    static constexpr std::size_t kDefaultMaxTextBytes = 4096;
    static constexpr std::size_t kDefaultMaxTotalTextBytes = std::size_t{256} * 1024;

    std::size_t maxEntries = kDefaultMaxEntries;
    std::size_t maxSessionHistoryEntries = kDefaultMaxSessionHistoryEntries;
    std::size_t maxActionsPerEntry = kDefaultMaxActionsPerEntry;
    std::size_t maxTextBytes = kDefaultMaxTextBytes;
    std::size_t maxTotalTextBytes = kDefaultMaxTotalTextBytes;
  };

  struct NotificationFeedUpdate final
  {
    std::uint64_t revision = 0;
    NotificationFeedMutationKind mutationKind = NotificationFeedMutationKind::Posted;
    std::vector<NotificationId> affectedIds{};
    // Automatic history eviction is part of the same committed revision as
    // the command that required it. Ids are listed from oldest to newest.
    std::vector<NotificationId> evictedIds{};
    // Service-produced updates always carry a non-null immutable snapshot
    // whose revision equals the update revision.
    std::shared_ptr<NotificationFeedState const> feedPtr{};
  };
} // namespace ao::rt
