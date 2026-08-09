// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <span>

namespace ao::library
{
  /** Full canonical validation for one serialized List record. */
  Result<> validateSerializedList(std::span<std::byte const> bytes);
} // namespace ao::library
