// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/ScopedRegistration.h>

namespace ao::audio
{
  /**
   * @brief A move-only handle that unsubscribes a listener when destroyed.
   */
  using Subscription = utility::ScopedRegistration;
} // namespace ao::audio
