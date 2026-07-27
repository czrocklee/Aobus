// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::winui
{
  class ScopedBooleanFlag final
  {
  public:
    explicit ScopedBooleanFlag(bool& value, bool const enabled = true) noexcept
      : _valuePtr{enabled ? &value : nullptr}, _previous{value}
    {
      if (_valuePtr != nullptr)
      {
        *_valuePtr = true;
      }
    }

    ~ScopedBooleanFlag() noexcept
    {
      if (_valuePtr != nullptr)
      {
        *_valuePtr = _previous;
      }
    }

    ScopedBooleanFlag(ScopedBooleanFlag const&) = delete;
    ScopedBooleanFlag& operator=(ScopedBooleanFlag const&) = delete;
    ScopedBooleanFlag(ScopedBooleanFlag&&) = delete;
    ScopedBooleanFlag& operator=(ScopedBooleanFlag&&) = delete;

  private:
    bool* _valuePtr = nullptr;
    bool _previous = false;
  };
} // namespace ao::winui
