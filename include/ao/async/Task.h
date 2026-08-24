// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Preload the GCC TSan fence guard before Asio; this header is used for its preprocessing effect.
#include <ao/async/detail/BoostAsioTsanPrelude.h>
#include <ao/compat/MoveOnlyFunction.h>

#include <boost/asio/awaitable.hpp>

#include <stop_token>

namespace ao::async
{
  template<typename T = void>
  using Task = boost::asio::awaitable<T>;

  using CancellableTask = compat::MoveOnlyFunction<Task<void>(std::stop_token)>;
} // namespace ao::async
