// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/DictionaryStore.h>

#include "TextAdmission.h"
#include "detail/LibraryError.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
{
  DictionaryStore::DictionaryStore(lmdb::IntegerKeyDatabase db,
                                   lmdb::ReadTransaction const& transaction,
                                   detail::LibraryIdentity const& identity)
    : _database{std::move(db)}
    , _identity{&identity}
    , _stringToId{0, DictHash{&_idToStringStorage}, DictEqual{&_idToStringStorage}}
  {
    auto const reader = _database.reader(transaction);
    _stringToId.reserve(reader.entryCount());

    for (auto const& [rawKey, buf] : reader)
    {
      if (rawKey.size() != sizeof(std::uint32_t))
      {
        detail::throwLibraryError(
          Error::Code::CorruptData, std::format("Dictionary database contains a {}-byte integer key", rawKey.size()));
      }

      std::uint32_t rawId = 0;
      std::memcpy(&rawId, rawKey.data(), sizeof(rawId));

      if (auto const expectedId = _idToStringStorage.size() + 1U; static_cast<std::size_t>(rawId) != expectedId)
      {
        detail::throwLibraryError(
          Error::Code::CorruptData,
          std::format("Dictionary ids are not the dense range 1..N: expected {}, found {}", expectedId, rawId));
      }

      auto const text = utility::bytes::stringView(buf);

      if (auto validationRes = detail::validatePersistedLibraryText(text, "Dictionary entry"); !validationRes)
      {
        auto error = std::move(validationRes.error());
        error.message = std::format("Dictionary entry {} failed text validation: {}", rawId, error.message);
        detail::throwLibraryError(std::move(error));
      }

      _idToStringStorage.emplace_back(text);

      if (!_stringToId.insert(DictionaryId{rawId}).second)
      {
        detail::throwLibraryError(Error::Code::CorruptData, "Dictionary database contains duplicate text values");
      }
    }

    _publishedSize = _idToStringStorage.size();
  }

  void DictionaryStore::refresh(lmdb::IntegerKeyDatabase::Reader const& reader)
  {
    auto const count = reader.entryCount();
    {
      auto const lock = std::shared_lock{_mutex};

      if (count <= _publishedSize)
      {
        return;
      }
    }

    // Only a snapshot that actually needs more ids takes this slow path.
    // If it observed the local native commit before publish(), wait for that
    // publication instead of appending its prepared tail a second time.
    auto const publicationLock = std::scoped_lock{_tailPublicationMutex};
    auto const lock = std::scoped_lock{_mutex};
    auto const previousSize = _publishedSize;

    if (count <= previousSize)
    {
      return;
    }

    _stringToId.reserve(count);

    try
    {
      for (auto index = previousSize; index < count; ++index)
      {
        auto const id = DictionaryId{static_cast<std::uint32_t>(index + 1U)};
        auto const optBytes = reader.get(id.raw());
        AO_INVARIANT(optBytes, "Committed dictionary tail is not dense");
        auto const text = utility::bytes::stringView(*optBytes);
        auto const validRes = detail::validatePersistedLibraryText(text, "Dictionary entry");
        AO_INVARIANT(validRes, "Committed dictionary tail contains invalid text");
        _idToStringStorage.emplace_back(text);
        auto const inserted = _stringToId.insert(id).second;
        AO_INVARIANT(inserted, "Committed dictionary tail rebinds existing text");
      }
    }
    catch (...)
    {
      while (_idToStringStorage.size() > previousSize)
      {
        _stringToId.erase(DictionaryId{static_cast<std::uint32_t>(_idToStringStorage.size())});
        _idToStringStorage.pop_back();
      }

      throw;
    }

    _publishedSize = count;
    ++_generation;
  }

  std::string_view DictionaryStore::get(DictionaryId id) const
  {
    auto const lock = std::shared_lock{_mutex};
    auto index = id.raw();

    // 0 is null/invalid
    AO_EXPECTS(index != 0, "Invalid dictionary ID");

    AO_EXPECTS(index <= _publishedSize, "Invalid dictionary ID");

    return _idToStringStorage[index - 1];
  }

  std::string_view DictionaryStore::getOrDefault(DictionaryId id, std::string_view defaultValue) const
  {
    auto const lock = std::shared_lock{_mutex};
    auto index = id.raw();

    if (index == 0 || index > _publishedSize)
    {
      return defaultValue;
    }

    return _idToStringStorage[index - 1];
  }

  std::optional<DictionaryId> DictionaryStore::findId(std::string_view str) const
  {
    auto const lock = std::shared_lock{_mutex};
    return findPublishedId(str);
  }

  std::optional<DictionaryId> DictionaryStore::findPublishedId(std::string_view text) const
  {
    if (auto const it = _stringToId.find(text); it != _stringToId.end() && it->raw() <= _publishedSize)
    {
      return *it;
    }

    return std::nullopt;
  }

  std::uint64_t DictionaryStore::generation() const
  {
    auto const lock = std::shared_lock{_mutex};
    return _generation;
  }

  std::size_t DictionaryStore::size() const
  {
    auto const lock = std::shared_lock{_mutex};
    return _publishedSize;
  }

  std::uint64_t DictionaryStore::bindSymbols(std::span<std::string const> symbols, std::span<DictionaryId> ids) const
  {
    AO_EXPECTS(symbols.size() == ids.size());
    auto const lock = std::shared_lock{_mutex};

    for (std::size_t index = 0; index < symbols.size(); ++index)
    {
      ids[index] = findPublishedId(symbols[index]).value_or(kInvalidDictionaryId);
    }

    return _generation;
  }

  struct DictionaryStore::Writer::Impl final
  {
    struct Delta final
    {
      DictionaryId id{};
      std::string text{};
    };

    Impl(DictionaryStore& owner, lmdb::WriteTransaction& writeTransaction)
      : dictionary{&owner}, transaction{&writeTransaction}
    {
      auto const lock = std::shared_lock{dictionary->_mutex};
      auto const committedSize = dictionary->_publishedSize;
      nextId =
        committedSize < std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(committedSize) + 1 : 0;
    }

    ~Impl() { rollbackPublication(); }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    Result<DictionaryId> intern(std::string_view value)
    {
      AO_EXPECTS(transaction->isActive(), "Dictionary writer transaction is no longer active");

      // The exact fast path avoids Unicode work for repeated values already
      // admitted by this store. A match is necessarily valid NFC because the
      // persisted and publication boundaries enforce that invariant.
      if (auto const optId = dictionary->findId(value); optId)
      {
        return *optId;
      }

      if (auto const it = overlay.find(value); it != overlay.end())
      {
        return it->second;
      }

      auto normalizedRes = detail::normalizeLibraryText(value, "Dictionary text");

      if (!normalizedRes)
      {
        return std::unexpected{normalizedRes.error()};
      }

      auto normalized = std::move(*normalizedRes);

      if (normalized != value)
      {
        if (auto const optId = dictionary->findId(normalized); optId)
        {
          return *optId;
        }

        if (auto const it = overlay.find(normalized); it != overlay.end())
        {
          return it->second;
        }
      }

      if (nextId == 0)
      {
        return makeError(Error::Code::ResourceExhausted, "Dictionary ID space exhausted");
      }

      if (!optWriter)
      {
        optWriter.emplace(dictionary->_database.writer(*transaction));
      }

      auto const id = DictionaryId{nextId};
      auto const [overlayIt, inserted] = overlay.emplace(std::move(normalized), id);
      AO_INVARIANT(inserted);

      try
      {
        delta.push_back(Delta{.id = id, .text = overlayIt->first});
      }
      catch (...)
      {
        overlay.erase(overlayIt);
        throw;
      }

      if (auto res = optWriter->create(id.raw(), utility::bytes::view(delta.back().text)); !res)
      {
        delta.pop_back();
        overlay.erase(overlayIt);
        return std::unexpected{res.error()};
      }

      nextId = nextId == std::numeric_limits<std::uint32_t>::max() ? 0 : nextId + 1;
      return id;
    }

    void preparePublication()
    {
      if (delta.empty())
      {
        return;
      }

      publicationLock = std::unique_lock{dictionary->_tailPublicationMutex};
      auto const lock = std::scoped_lock{dictionary->_mutex};
      auto const expectedFirstId = static_cast<std::uint32_t>(dictionary->_publishedSize) + 1;

      AO_INVARIANT(
        delta.front().id.raw() == expectedFirstId, "Dictionary publication order changed during a library write");

      // Perform every allocation before native commit. Readers filter both
      // indices by _publishedSize, including if preparation throws partway.
      // WriteTransaction rolls that hidden tail back before releasing the gate.
      dictionary->_stringToId.reserve(dictionary->_stringToId.size() + delta.size());

      for (auto& entry : delta)
      {
        dictionary->_idToStringStorage.emplace_back(std::move(entry.text));
      }

      for (auto const& entry : delta)
      {
        auto const [it, inserted] = dictionary->_stringToId.insert(entry.id);
        AO_INVARIANT(inserted, "Dictionary publication would rebind an existing string");
        insertedCount++;
      }
    }

    void publish() noexcept
    {
      if (!publicationLock.owns_lock())
      {
        return;
      }

      {
        auto const lock = std::scoped_lock{dictionary->_mutex};
        dictionary->_publishedSize = dictionary->_idToStringStorage.size();
        dictionary->_generation++;
      }
      publicationLock.unlock();
    }

    void rollbackPublication() noexcept
    {
      if (!publicationLock.owns_lock())
      {
        return;
      }

      {
        auto const lock = std::scoped_lock{dictionary->_mutex};

        for (std::size_t index = 0; index < insertedCount; ++index)
        {
          dictionary->_stringToId.erase(delta[index].id);
        }

        while (dictionary->_idToStringStorage.size() > dictionary->_publishedSize)
        {
          dictionary->_idToStringStorage.pop_back();
        }
      }
      publicationLock.unlock();
    }

    DictionaryStore* dictionary;
    lmdb::WriteTransaction* transaction;
    std::optional<lmdb::IntegerKeyDatabase::Writer> optWriter;
    std::map<std::string, DictionaryId, std::less<>> overlay;
    std::vector<Delta> delta;
    std::unique_lock<std::mutex> publicationLock;
    std::uint32_t nextId = 1;
    std::size_t insertedCount = 0;
  };

  DictionaryStore::Writer::Writer(DictionaryStore& dictionary, lmdb::WriteTransaction& transaction)
    : _implPtr{std::make_unique<Impl>(dictionary, transaction)}
  {
  }

  DictionaryStore::Writer::~Writer() = default;
  DictionaryStore::Writer::Writer(Writer&&) noexcept = default;
  DictionaryStore::Writer& DictionaryStore::Writer::operator=(Writer&&) noexcept = default;

  Result<DictionaryId> DictionaryStore::Writer::intern(std::string_view value)
  {
    return _implPtr->intern(value);
  }

  void DictionaryStore::Writer::preparePublication()
  {
    _implPtr->preparePublication();
  }

  void DictionaryStore::Writer::publish() noexcept
  {
    _implPtr->publish();
  }

  void DictionaryStore::Writer::rollbackPublication() noexcept
  {
    _implPtr->rollbackPublication();
  }

  DictionaryReadCache::DictionaryReadCache(DictionaryStore const& dictionary)
    : _dictionary{&dictionary}
  {
  }

  std::string_view DictionaryReadCache::get(DictionaryId id)
  {
    if (_entriesPtr != nullptr && id != kInvalidDictionaryId)
    {
      if (auto const& entry = (*_entriesPtr)[id.raw() % kCapacity]; entry.id == id)
      {
        return entry.value;
      }
    }

    auto const value = _dictionary->get(id);

    if (!value.empty())
    {
      if (_entriesPtr == nullptr)
      {
        _entriesPtr = std::make_unique<std::array<Entry, kCapacity>>();
      }

      (*_entriesPtr)[id.raw() % kCapacity] = Entry{.id = id, .value = value};
    }

    return value;
  }

  DictionaryStore const& DictionaryReadCache::dictionary() const noexcept
  {
    return *_dictionary;
  }

  DictionaryReadContext::DictionaryReadContext(DictionaryStore const& dictionary)
    : _dictionary{&dictionary}
  {
  }

  DictionaryReadContext::DictionaryReadContext(DictionaryReadCache& cache)
    : _dictionary{&cache.dictionary()}, _cache{&cache}
  {
  }

  std::uint64_t DictionaryReadContext::bind(std::span<std::string const> symbols, std::span<DictionaryId> ids) const
  {
    return _dictionary->bindSymbols(symbols, ids);
  }

  std::string_view DictionaryReadContext::get(DictionaryId id)
  {
    return _cache != nullptr ? _cache->get(id) : _dictionary->get(id);
  }

  DictionaryStore const& DictionaryReadContext::dictionary() const noexcept
  {
    return *_dictionary;
  }
} // namespace ao::library
