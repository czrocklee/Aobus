// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Exception.h>
#include <ao/Type.h>
#include <ao/library/ResourceStore.h>
#include <ao/lmdb/Transaction.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ao::library
{
  using Writer = ResourceStore::Writer;

  Writer ResourceStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_database.writer(txn)};
  }

  namespace
  {
    // FNV-1a 32-bit hash
    // Created by Glenn Fowler, Landon Curt Noll, and Peter Vo in 1991
    // Simple, fast, and good distribution for content-addressable storage
    ResourceId fnv1a(std::span<std::byte const> data)
    {
      constexpr std::uint32_t kFnvOffsetBasis = 2166136261U;
      constexpr std::uint32_t kFnvPrime = 16777619U;

      std::uint32_t hash = kFnvOffsetBasis;

      for (std::byte const byte : data)
      {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnvPrime;
      }

      return ResourceId{hash};
    }
  }

  ResourceId Writer::create(std::span<std::byte const> data)
  {
    ResourceId key = fnv1a(data);

    if (key == kInvalidResourceId)
    {
      key = ResourceId{1};
    }

    auto const firstKey = key;

    while (true)
    {
      auto optValue = _writer.get(key.raw());

      if (!optValue)
      {
        _writer.create(key.raw(), data);
        return key;
      }

      if (std::ranges::equal(*optValue, data)) [[likely]]
      {
        return key;
      }

      if (key == ResourceId{std::numeric_limits<std::uint32_t>::max()})
      {
        key = ResourceId{1};
      }
      else
      {
        ++key;
      }

      if (key == firstKey)
      {
        break;
      }
    }

    ao::throwException<Exception>("Hash table full");
  }
}
