// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/StrongType.h>

#include <cstdint>

namespace ao::rt
{
  using ViewId = utility::StrongType<std::uint64_t, struct ViewIdTag>;

  inline constexpr auto kInvalidViewId = ViewId{0};
} // namespace ao::rt
