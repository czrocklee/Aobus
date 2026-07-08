// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>

namespace ao::rt
{
  using NotificationId = utility::StrongType<std::uint64_t, struct NotificationIdTag>;

  inline constexpr auto kInvalidNotificationId = NotificationId{0};
} // namespace ao::rt
