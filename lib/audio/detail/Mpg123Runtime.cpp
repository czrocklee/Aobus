// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/detail/Mpg123Runtime.h>

#include <mpg123.h>

#include <cstdint>
#include <mutex>

namespace ao::audio::detail
{
  namespace
  {
    struct Mpg123RuntimeState final
    {
      std::mutex mutex;
      std::uint32_t refCount = 0;
    };

    Mpg123RuntimeState& mpg123RuntimeState()
    {
      static Mpg123RuntimeState state;
      return state;
    }
  } // namespace

  Mpg123EnvironmentGuard::Mpg123EnvironmentGuard()
  {
    auto& state = mpg123RuntimeState();
    auto const lock = std::scoped_lock{state.mutex};

    if (state.refCount == 0 && ::mpg123_init() != MPG123_OK)
    {
      return;
    }

    ++state.refCount;
    _active = true;
  }

  Mpg123EnvironmentGuard::~Mpg123EnvironmentGuard() noexcept
  {
    if (!_active)
    {
      return;
    }

    auto& state = mpg123RuntimeState();
    auto const lock = std::scoped_lock{state.mutex};
    --state.refCount;

    if (state.refCount == 0)
    {
      ::mpg123_exit();
    }
  }
} // namespace ao::audio::detail
