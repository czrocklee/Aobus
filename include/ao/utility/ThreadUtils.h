// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#ifdef __linux__
#include <pthread.h>
#endif

namespace ao
{
  inline void setCurrentThreadName(std::string_view name) noexcept
  {
#ifdef __linux__
    auto buf = std::array<char, 16>{};
    std::size_t const len = std::min(name.size(), buf.size() - 1);
    std::memcpy(buf.data(), name.data(), len);
    ::pthread_setname_np(pthread_self(), buf.data());
#endif
  }
}