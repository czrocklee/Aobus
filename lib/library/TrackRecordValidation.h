// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <span>

namespace ao::library
{
  /** Full canonical validation for serialized Track record sides. */
  Result<> validateSerializedHotTrack(std::span<std::byte const> bytes);
  Result<> validateSerializedColdTrack(std::span<std::byte const> bytes);
  Result<> validateSerializedTrackReferences(std::span<std::byte const> hotBytes,
                                             std::span<std::byte const> coldBytes,
                                             std::size_t dictionarySize);
} // namespace ao::library
