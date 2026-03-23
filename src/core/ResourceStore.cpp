// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/Exception.h>
#include <rs/core/ResourceStore.h>

#include <cstdint>
#include <cstring>
#include <rs/Exception.h>
#include <span>

namespace rs::core
{
  using Writer = ResourceStore::Writer;

  Writer ResourceStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_database.reader(txn), _database.writer(txn)};
  }

  namespace
  {
    // FNV-1a 32-bit hash
    // Created by Glenn Fowler, Landon Curt Noll, and Peter Vo in 1991
    // Simple, fast, and good distribution for content-addressable storage
    std::uint32_t fnv1a(std::span<std::byte const> data)
    {
      constexpr std::uint32_t kFnvOffsetBasis = 2166136261U;
      constexpr std::uint32_t kFnvPrime = 16777619U;

      std::uint32_t hash = kFnvOffsetBasis;

      for (std::byte b : data)
      {
        hash ^= static_cast<std::uint8_t>(b);
        hash *= kFnvPrime;
      }

      return hash;
    }
  }

  ResourceId Writer::create(std::span<std::byte const> buffer)
  {
    for (std::uint32_t key = fnv1a(buffer);; ++key)
    {
      auto optValue = _writer.get(key);

      if (!optValue)
      {
        _writer.create(key, buffer);
        return ResourceId{key};
      }

      if (std::ranges::equal(*optValue, buffer)) [[likely]] { return ResourceId{key}; }

      // Prevent infinite loop (though extremely unlikely with 32-bit hash space)
      if (key == std::numeric_limits<std::uint32_t>::max()) { RS_THROW(Exception, "Hash table full"); }
    }
  }
}
