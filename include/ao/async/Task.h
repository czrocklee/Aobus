// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Preload the GCC TSan fence guard before Asio; this header is used for its preprocessing effect.
// NOLINTNEXTLINE(misc-include-cleaner)
#include <ao/async/detail/BoostAsioTsanPrelude.h>

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <stop_token>

namespace ao::async
{
  template<typename T = void>
  using Task = boost::asio::awaitable<T>;

  using CancellableTask = std::move_only_function<Task<void>(std::stop_token)>;
} // namespace ao::async
