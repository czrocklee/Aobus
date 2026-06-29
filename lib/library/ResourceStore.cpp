// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ResourceStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/Fnv1a.h>

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
    // 32-bit FNV-1a: simple, fast, and good distribution for content-addressable storage.
    auto key = ResourceId{utility::fnv1a32(data)};

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
