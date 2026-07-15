// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <exception>
#include <functional>
#include <string_view>

namespace ao::async
{
  /**
   * Consumes an unexpected exception at an asynchronous ownership boundary.
   *
   * The handler may be called concurrently from runtime worker threads. The
   * context view is valid only for the duration of the call.
   */
  using AsyncExceptionHandler = std::function<void(std::exception_ptr, std::string_view context)>;
} // namespace ao::async
