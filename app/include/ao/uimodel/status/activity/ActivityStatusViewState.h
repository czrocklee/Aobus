// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationState.h>

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
} // namespace ao::uimodel
