// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/ScopedBooleanFlag.h"

namespace ao::winui
{
  ScopedBooleanFlag::ScopedBooleanFlag(bool& value, bool const enabled) noexcept
    : _value{enabled ? &value : nullptr}, _previous{value}
  {
    if (_value != nullptr)
    {
      *_value = true;
    }
  }

  ScopedBooleanFlag::~ScopedBooleanFlag()
  {
    if (_value != nullptr)
    {
      *_value = _previous;
    }
  }
} // namespace ao::winui
