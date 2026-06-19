// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <functional>
#include <utility>

namespace ao::utility
{
  class ScopedRegistration final
  {
  public:
    ScopedRegistration() = default;

    explicit ScopedRegistration(std::move_only_function<void()> unregister)
      : _unregister{std::move(unregister)}
    {
    }

    ~ScopedRegistration()
    {
      if (_unregister)
      {
        _unregister();
      }
    }

    ScopedRegistration(ScopedRegistration const&) = delete;
    ScopedRegistration& operator=(ScopedRegistration const&) = delete;

    ScopedRegistration(ScopedRegistration&&) noexcept = default;
    ScopedRegistration& operator=(ScopedRegistration&& other) noexcept
    {
      if (this != &other)
      {
        reset();
        _unregister = std::move(other._unregister);
      }

      return *this;
    }

    void reset()
    {
      if (_unregister)
      {
        auto unregister = std::move(_unregister);
        unregister();
      }
    }

    explicit operator bool() const noexcept { return static_cast<bool>(_unregister); }

  private:
    std::move_only_function<void()> _unregister;
  };
} // namespace ao::utility
