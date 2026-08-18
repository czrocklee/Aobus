// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ResourceStore.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/WriteTransaction.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Sha256.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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

  namespace
  {
    /// The open gate admits only 36-byte values, so a row that does not parse
    /// after that point is a storage fault rather than a caller error.
    ResourceDescriptor validatedDescriptor(std::span<std::byte const> const row, std::uint32_t const rawId)
    {
      auto const optDescriptor = parseResourceDescriptor(row);
      AO_INVARIANT(optDescriptor,
                   "Resource {} holds {} bytes rather than a descriptor after library validation",
                   rawId,
                   row.size());
      return *optDescriptor;
    }
  } // namespace

  std::optional<ResourceDescriptor> ResourceStore::Reader::get(ResourceId const id) const
  {
    auto const optRow = _reader.get(id.raw());

    if (!optRow)
    {
      return std::nullopt;
    }

    return validatedDescriptor(*optRow, id.raw());
  }

  void ResourceStore::Reader::Iterator::refresh() const
  {
    auto const rawId = static_cast<std::uint32_t>(_iterator->first);
    AO_INVARIANT(rawId != 0, "Resource iterator encountered the reserved id zero after library validation");
    _value = {ResourceId{rawId}, validatedDescriptor(_iterator->second, rawId)};
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

  std::optional<ResourceDescriptor> ResourceStore::Writer::get(ResourceId const id) const
  {
    auto const optRow = _writer.get(id.raw());

    if (!optRow)
    {
      return std::nullopt;
    }

    return validatedDescriptor(*optRow, id.raw());
  }

  Result<ResourceId> Writer::create(std::span<std::byte const> const data)
  {
    if (!resourceByteLengthFits(data.size()))
    {
      return makeError(Error::Code::ValueTooLarge,
                       std::format("Resource content of {} bytes exceeds the stored length field", data.size()));
    }

    return store(
      ResourceDescriptor{.digest = utility::computeSha256(data), .byteLength = static_cast<std::uint32_t>(data.size())},
      LengthEvidence::Counted);
  }

  Result<ResourceId> Writer::getOrCreate(ResourceDescriptor const& descriptor)
  {
    return store(descriptor, LengthEvidence::Declared);
  }

  Result<ResourceId> Writer::store(ResourceDescriptor const& descriptor, LengthEvidence const evidence)
  {
    // The digest is the identity; the id is a 32-bit handle for it, so the probe
    // resolves handle collisions by comparing digests rather than content.
    auto key = deriveResourceId(descriptor.digest);
    auto const firstKey = key;

    while (true)
    {
      auto const optExisting = _writer.get(key.raw());

      if (!optExisting)
      {
        // Slot is free: this content has not been stored under this key yet.
        if (auto createRes = _writer.create(key.raw(), utility::bytes::view(descriptor)); !createRes)
        {
          return std::unexpected{createRes.error()};
        }

        return key;
      }

      if (auto const stored = validatedDescriptor(*optExisting, key.raw()); stored.digest == descriptor.digest)
        [[likely]]
      {
        if (evidence == LengthEvidence::Counted && stored.byteLength != descriptor.byteLength)
        {
          // Counted bytes outrank whatever the row held, which is how a length
          // a document guessed wrong gets repaired.
          if (auto updateRes = _writer.update(key.raw(), utility::bytes::view(descriptor)); !updateRes)
          {
            return std::unexpected{updateRes.error()};
          }
        }

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
