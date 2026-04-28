// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <thread>
#include <string_view>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace app::core::util
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