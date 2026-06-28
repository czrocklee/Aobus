// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/StateTypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  enum class ActivityStatusKind : std::uint8_t
  {
    Idle,
    Processing,
    Success,
    Info,
    Warning,
    Error,
  };

  struct ActivityActionView final
  {
    std::string id{};
    std::string label{};
  };

  struct ActivityActionAvailability final
  {
    bool visible = false;
    bool enabled = false;
    std::string label{};
    std::string disabledReason{};
  };

  struct ActivityResolvedActionView final
  {
    std::string id{};
    bool enabled = false;
    std::string label{};
    std::string disabledReason{};
  };

  using ActivityActionAvailabilityResolver =
    std::function<ActivityActionAvailability(std::string_view, std::string_view)>;

  struct ActivityCompactState final
  {
    ActivityStatusKind kind = ActivityStatusKind::Idle;
    std::string text{};
    std::optional<double> optProgressFraction{};
    std::size_t groupedCount = 0;
    bool persistent = false;
    bool dismissible = false;
    bool hasDetails = false;
    std::optional<std::chrono::milliseconds> optAutoDismissTimeout{};
    std::vector<rt::NotificationId> sourceNotificationIds{};
  };

  struct ActivityDetailItem final
  {
    rt::NotificationId id{};
    rt::NotificationSeverity severity = rt::NotificationSeverity::Info;
    std::string title{};
    std::string message{};
    std::string iconName{};
    bool sticky = false;
    bool dismissible = false;
    std::optional<rt::NotificationProgressMode> optProgressMode{};
    double progressFraction = 0.0;
    std::string progressLabel{};
    std::vector<ActivityActionView> actions{};
  };

  struct ActivityTaskDetail final
  {
    std::string message{};
    double progressFraction = 0.0;
  };

  struct ActivityDetailState final
  {
    std::vector<ActivityDetailItem> items{};
    std::optional<ActivityTaskDetail> optLibraryTask{};
    bool hasActiveProgress = false;
  };

  struct ActivityStatusViewState final
  {
    ActivityCompactState compact{};
    ActivityDetailState detail{};
  };

  inline constexpr std::chrono::milliseconds kActivityStatusDefaultAutoDismissTimeout{5000};

  std::string_view activityStatusKindCssClass(ActivityStatusKind kind);
  bool hasDetailContent(ActivityDetailState const& detail) noexcept;
  std::vector<ActivityResolvedActionView> resolveActivityActionViews(
    std::vector<ActivityActionView> const& actions,
    ActivityActionAvailabilityResolver const& resolveAction,
    std::size_t maxVisibleActions);

  class ActivityStatusModel final
  {
  public:
    void initialize(rt::NotificationFeedState const& feed);
    void onFeedChanged(rt::NotificationFeedState const& feed);
    void onNotificationPosted(rt::NotificationFeedState const& feed, rt::NotificationId id);
    void onLibraryTaskProgress(std::string message, double fraction);
    void onLibraryTaskCompleted(std::size_t count, rt::NotificationFeedState const& feed);
    void dismissCompact(rt::NotificationFeedState const& feed);
    void hideDetailNotificationFromActivity(rt::NotificationId id, rt::NotificationFeedState const& feed);
    void onTransientExpired(rt::NotificationFeedState const& feed);

    std::vector<rt::NotificationId> locallyHideableNotificationIds(rt::NotificationFeedState const& feed) const;
    ActivityStatusViewState const& viewState() const noexcept;

  private:
    struct LibraryProgressState final
    {
      std::string message{};
      double fraction = 0.0;
    };

    void projectDetail(rt::NotificationFeedState const& feed);
    void projectPersistentCompact(rt::NotificationFeedState const& feed);
    void showNotificationCompact(rt::NotificationEntry const& entry);
    void showCompletionCompact(std::size_t count);
    bool compactSourceStillExists(ActivityCompactState const& compact, rt::NotificationFeedState const& feed) const;
    bool compactSourceIsDismissed(ActivityCompactState const& compact) const;
    bool compactSourceIsSuppressed(rt::NotificationId id) const;
    void rememberDismissedCompactSources();
    void pruneDismissedSources(rt::NotificationFeedState const& feed);

    ActivityStatusViewState _state{};
    bool _taskActive = false;
    std::optional<LibraryProgressState> _optLibraryProgress{};
    std::optional<rt::NotificationEntry> _optDeferredNotification{};
    std::vector<rt::NotificationId> _compactDismissedNotificationIds{};
    std::vector<rt::NotificationId> _detailDismissedNotificationIds{};
  };
} // namespace ao::uimodel
