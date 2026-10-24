// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::winui
{
  class ScopedBooleanFlag final
  {
  public:
    explicit ScopedBooleanFlag(bool& value, bool enabled = true) noexcept;
    ~ScopedBooleanFlag();

    ScopedBooleanFlag(ScopedBooleanFlag const&) = delete;
    ScopedBooleanFlag& operator=(ScopedBooleanFlag const&) = delete;
    ScopedBooleanFlag(ScopedBooleanFlag&&) = delete;
    ScopedBooleanFlag& operator=(ScopedBooleanFlag&&) = delete;

  private:
    bool* _value = nullptr;
    bool _previous = false;
  };
} // namespace ao::winui
