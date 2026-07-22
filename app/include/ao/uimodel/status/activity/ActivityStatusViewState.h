// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
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

  struct ActivityCompactState final
  {
    ActivityStatusKind kind = ActivityStatusKind::Idle;
    std::string text{};
    std::optional<double> optProgressFraction{};
    bool dismissible = false;
    bool hasDetails = false;
    std::optional<std::chrono::milliseconds> optAutoDismissTimeout{};
  };

  struct ActivityDetailItem final
  {
    rt::NotificationId id{};
    rt::NotificationSeverity severity = rt::NotificationSeverity::Info;
    std::string message{};
    bool dismissible = false;
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
  };

  struct ActivityStatusViewState final
  {
    ActivityCompactState compact{};
    ActivityDetailState detail{};
  };

  inline constexpr std::chrono::milliseconds kActivityStatusDefaultAutoDismissTimeout{5000};

  bool hasDetailContent(ActivityDetailState const& detail) noexcept;
} // namespace ao::uimodel
