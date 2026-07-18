// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>
#include <string>

namespace ao::rt
{
  using NotificationId = utility::StrongType<std::uint64_t, struct NotificationIdTag>;
  using NotificationReportKey = utility::StrongType<std::string, struct NotificationReportKeyTag>;

  inline constexpr auto kInvalidNotificationId = NotificationId{0};
} // namespace ao::rt
