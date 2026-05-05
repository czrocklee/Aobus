// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <string_view>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace ao
{
  inline void setCurrentThreadName(std::string_view name) noexcept
  {
#if defined(__linux__)
    ::pthread_setname_np(pthread_self(), name.data());
#elif defined(__APPLE__)
    ::pthread_setname_np(name.data());
#endif
  }
}