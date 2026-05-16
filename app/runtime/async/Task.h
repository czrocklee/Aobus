// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <boost/asio/awaitable.hpp>

namespace ao::rt::async
{
  template<typename T = void>
  using Task = boost::asio::awaitable<T>;
} // namespace ao::rt::async
