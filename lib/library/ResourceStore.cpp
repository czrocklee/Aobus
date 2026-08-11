// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ResourceStore.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>
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

  ResourceStore::Reader ResourceStore::reader(ReadTransaction const& transaction) const
  {
    return Reader{_database.reader(transaction.native(*_identity))};
  }

  ResourceStore::Reader ResourceStore::reader(WriteTransaction const& transaction) const
  {
    return Reader{_database.reader(transaction.native(*_identity))};
  }

  ResourceStore::Reader ResourceStore::reader(LibraryWrite const& write) const
  {
    return Reader{_database.reader(write.native(*_identity))};
  }

  Writer ResourceStore::writer(WriteTransaction& transaction) const
  {
    return Writer{_database.writer(transaction.native(*_identity))};
  }

  std::optional<std::span<std::byte const>> ResourceStore::Reader::get(ResourceId const id) const
  {
    auto optResult = _reader.get(id.raw());
    AO_INVARIANT(!optResult || !optResult->empty(), "Resource {} is empty after library validation", id.raw());
    return optResult;
  }

  void ResourceStore::Reader::Iterator::refresh() const
  {
    auto const rawId = static_cast<std::uint32_t>(_iterator->first);
    AO_INVARIANT(rawId != 0, "Resource iterator encountered the reserved id zero after library validation");
    AO_INVARIANT(!_iterator->second.empty(), "Resource {} is empty after library validation", rawId);
    _value = {ResourceId{rawId}, _iterator->second};
  }

  ResourceStore::Reader::Iterator::reference ResourceStore::Reader::Iterator::operator*() const
  {
    refresh();
    return _value;
  }

  ResourceStore::Reader::Iterator::pointer ResourceStore::Reader::Iterator::operator->() const
  {
    refresh();
    return &_value;
  }

  ResourceStore::Reader::Iterator& ResourceStore::Reader::Iterator::operator++()
  {
    ++_iterator;
    return *this;
  }

  std::optional<std::span<std::byte const>> ResourceStore::Writer::get(ResourceId const id) const
  {
    auto optResult = _writer.get(id.raw());
    AO_INVARIANT(!optResult || !optResult->empty(), "Resource {} is empty after library validation", id.raw());
    return optResult;
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
        if (auto createRes = _writer.create(key.raw(), data); !createRes)
        {
          return std::unexpected{createRes.error()};
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
        key = ResourceId{key.raw() + 1U};
      }

      if (key == firstKey)
      {
        break;
      }
    }

    return makeError(Error::Code::ResourceExhausted, "Resource ID space exhausted");
  }
} // namespace ao::library
