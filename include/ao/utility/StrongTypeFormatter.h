// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <format>

namespace std
{
  template<typename T, typename Tag>
  // NOLINTNEXTLINE(bugprone-std-namespace-modification) -- permitted user-type formatter specialization
  struct formatter<ao::utility::StrongType<T, Tag>> : formatter<T>
  {
    auto format(ao::utility::StrongType<T, Tag> const& id, format_context& ctx) const
    {
      return formatter<T>::format(id.raw(), ctx);
    }
  };
} // namespace std
