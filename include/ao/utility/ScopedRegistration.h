// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>

namespace ao::utility
{
  class ScopedRegistration final
  {
  public:
    ScopedRegistration() = default;
    explicit ScopedRegistration(std::move_only_function<void()> unregister);
    ~ScopedRegistration();

    ScopedRegistration(ScopedRegistration const&) = delete;
    ScopedRegistration& operator=(ScopedRegistration const&) = delete;

    ScopedRegistration(ScopedRegistration&&) noexcept;
    ScopedRegistration& operator=(ScopedRegistration&& other) noexcept;

    void reset();

    explicit operator bool() const noexcept { return static_cast<bool>(_unregister); }

  private:
    std::move_only_function<void()> _unregister;
  };
} // namespace ao::utility
