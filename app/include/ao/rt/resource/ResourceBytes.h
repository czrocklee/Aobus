// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ao::rt
{
  /**
   * Immutable, independently owned encoded resource bytes.
   *
   * Copies share the same storage and remain valid after the loader or cache
   * that produced them is destroyed or evicts its entry.
   */
  class ResourceBytes final
  {
  public:
    using Storage = std::vector<std::byte>;

    ResourceBytes() = default;

    explicit ResourceBytes(Storage bytes)
      : _storagePtr{bytes.empty() ? nullptr : std::make_shared<Storage const>(std::move(bytes))}
    {
    }

    bool empty() const noexcept { return !_storagePtr || _storagePtr->empty(); }

    std::span<std::byte const> view() const noexcept
    {
      return _storagePtr ? std::span<std::byte const>{*_storagePtr} : std::span<std::byte const>{};
    }

  private:
    std::shared_ptr<Storage const> _storagePtr;
  };
} // namespace ao::rt
