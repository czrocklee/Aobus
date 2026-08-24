// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/compat/MoveOnlyFunction.h>

namespace ao::utility
{
  class [[nodiscard]] ScopedRegistration final
  {
  public:
    ScopedRegistration() = default;
    explicit ScopedRegistration(compat::MoveOnlyFunction<void()> unregister);
    ~ScopedRegistration();

    ScopedRegistration(ScopedRegistration const&) = delete;
    ScopedRegistration& operator=(ScopedRegistration const&) = delete;

    ScopedRegistration(ScopedRegistration&&) noexcept;
    ScopedRegistration& operator=(ScopedRegistration&& other) noexcept;

    void reset();

    explicit operator bool() const noexcept { return static_cast<bool>(_unregister); }

  private:
    compat::MoveOnlyFunction<void()> _unregister;
  };
} // namespace ao::utility
