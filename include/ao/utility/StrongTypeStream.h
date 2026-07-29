// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <ostream>
#include <type_traits>

namespace ao::utility
{
  template<typename T, typename Tag>
  std::ostream& operator<<(std::ostream& os, StrongType<T, Tag> const& value)
    requires std::is_integral_v<T>
  {
    return os << value.raw();
  }
} // namespace ao::utility
