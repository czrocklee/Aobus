// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

namespace ao::audio::detail
{
  class [[nodiscard]] Mpg123EnvironmentGuard final
  {
  public:
    Mpg123EnvironmentGuard();
    ~Mpg123EnvironmentGuard() noexcept;

    Mpg123EnvironmentGuard(Mpg123EnvironmentGuard const&) = delete;
    Mpg123EnvironmentGuard& operator=(Mpg123EnvironmentGuard const&) = delete;

    Mpg123EnvironmentGuard(Mpg123EnvironmentGuard&&) = delete;
    Mpg123EnvironmentGuard& operator=(Mpg123EnvironmentGuard&&) = delete;

  private:
    bool _active = false;
  };
} // namespace ao::audio::detail
