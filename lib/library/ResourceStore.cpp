// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ResourceStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/Xxh3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>

namespace ao::library
{
  using Writer = ResourceStore::Writer;

  Writer ResourceStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_database.writer(txn)};
  }

  Result<ResourceId> Writer::create(std::span<std::byte const> data)
  {
    // Low 32 bits of one-shot XXH3-64: fast and well distributed for
    // content-addressable storage; the probe-and-verify loop below resolves
    // collisions in the 32-bit key space.
    auto key = ResourceId{static_cast<std::uint32_t>(utility::xxh3Hash64(data))};

    if (key == kInvalidResourceId)
    {
      key = ResourceId{1};
    }

    auto const firstKey = key;

    while (true)
    {
      auto optExisting = _writer.get(key.raw());

      if (!optExisting)
      {
        // Slot is free: this content has not been stored under this key yet.
        if (auto createResult = _writer.create(key.raw(), data); !createResult)
        {
          return std::unexpected{createResult.error()};
        }

        return key;
      }

      if (std::ranges::equal(*optExisting, data)) [[likely]]
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

    return makeError(Error::Code::ResourceExhausted, "Resource ID space exhausted");
  }
} // namespace ao::library
