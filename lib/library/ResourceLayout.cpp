// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/ResourceLayout.h>

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

namespace ao::library
{
  std::optional<ResourceDescriptor> parseResourceDescriptor(std::span<std::byte const> const row) noexcept
  {
    if (row.size() != kResourceDescriptorSize)
    {
      return std::nullopt;
    }

    auto descriptor = ResourceDescriptor{};
    std::memcpy(&descriptor, row.data(), sizeof(descriptor));
    return descriptor;
  }
} // namespace ao::library
