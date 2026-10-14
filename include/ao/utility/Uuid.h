// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ao::utility
{
  constexpr std::size_t kUuidByteCount = 16;

  using UuidBytes = std::array<std::byte, kUuidByteCount>;

  std::string formatUuid(UuidBytes const& id);

  /** Parses the canonical 8-4-4-4-12 form; hexadecimal digits may use either case. */
  std::optional<UuidBytes> parseUuid(std::string_view text);
} // namespace ao::utility
