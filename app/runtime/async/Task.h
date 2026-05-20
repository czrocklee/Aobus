// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>

namespace ao::rt::async
{
  template<typename T = void>
  using Task = boost::asio::awaitable<T>;

  using CancellationSignal = boost::asio::cancellation_signal;
  using CancellationSlot = boost::asio::cancellation_slot;
  using CancellationType = boost::asio::cancellation_type;
} // namespace ao::rt::async
