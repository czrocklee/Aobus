// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

namespace ao::winui
{
  class ScopedBooleanFlag final
  {
  public:
    explicit ScopedBooleanFlag(bool& value, bool const enabled = true) noexcept
      : _value{enabled ? &value : nullptr}, _previous{value}
    {
      if (_value != nullptr)
      {
        *_value = true;
      }
    }

    ~ScopedBooleanFlag()
    {
      if (_value != nullptr)
      {
        *_value = _previous;
      }
    }

    ScopedBooleanFlag(ScopedBooleanFlag const&) = delete;
    ScopedBooleanFlag& operator=(ScopedBooleanFlag const&) = delete;
    ScopedBooleanFlag(ScopedBooleanFlag&&) = delete;
    ScopedBooleanFlag& operator=(ScopedBooleanFlag&&) = delete;

  private:
    bool* _value = nullptr;
    bool _previous = false;
  };
} // namespace ao::winui
