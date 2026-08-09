// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/library/MetadataLayout.h>

#include <cstdint>
#include <shared_mutex>

namespace ao::library
{
  class WriteTransaction;
}

namespace ao::library::detail
{
  struct MetadataSnapshot final
  {
    MetadataHeader header{};
    std::uint64_t revision = 0;
  };

  /** Process-local publication state for logical library metadata values. */
  class MetadataState final
  {
  public:
    MetadataState(MetadataHeader header, std::uint64_t revision)
      : _header{header}, _revision{revision}
    {
    }

    MetadataSnapshot snapshot() const;

  private:
    friend class ::ao::library::WriteTransaction;

    mutable std::shared_mutex _mutex;
    MetadataHeader _header{};
    std::uint64_t _revision = 0;
  };
} // namespace ao::library::detail
