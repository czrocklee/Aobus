// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <gsl-lite/gsl-lite.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
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
  DictionaryStore::DictionaryStore(lmdb::Database db,
                                   lmdb::ReadTransaction const& transaction,
                                   detail::LibraryIdentity const& identity)
    : _database{std::move(db)}
    , _identity{&identity}
    , _stringToId{0, DictHash{&_idToStringStorage}, DictEqual{&_idToStringStorage}}
  {
    auto const reader = _database.reader(transaction);
    _stringToId.reserve(reader.entryCount());

    for (auto const& [id, buf] : reader)
    {
      auto const rawId = static_cast<std::uint32_t>(id);
      _idToStringStorage.emplace_back(utility::bytes::stringView(buf));
      _stringToId.insert(DictionaryId{rawId});
    }
  }

  std::string_view DictionaryStore::get(DictionaryId id) const
  {
    auto const lock = std::shared_lock{_mutex};
    auto index = id.raw();

    // 0 is null/invalid
    if (index == 0)
    {
      ao::throwException<Exception>("Invalid dictionary ID");
    }

    if (index - 1 >= _idToStringStorage.size())
    {
      ao::throwException<Exception>("Invalid dictionary ID");
    }

    return _idToStringStorage[index - 1];
  }

  std::string_view DictionaryStore::getOrDefault(DictionaryId id, std::string_view defaultValue) const
  {
    auto const lock = std::shared_lock{_mutex};
    auto index = id.raw();

    if (index == 0 || index - 1 >= _idToStringStorage.size())
    {
      return defaultValue;
    }

    return _idToStringStorage[index - 1];
  }

  DictionaryId DictionaryStore::lookupId(std::string_view str) const
  {
    auto const lock = std::shared_lock{_mutex};

    if (auto it = _stringToId.find(str); it != _stringToId.end())
    {
      return *it;
    }

    ao::throwException<Exception>("String not found in dictionary");
  }

  std::optional<DictionaryId> DictionaryStore::findId(std::string_view str) const
  {
    auto const lock = std::shared_lock{_mutex};

    if (auto const it = _stringToId.find(str); it != _stringToId.end())
    {
      return *it;
    }

    return std::nullopt;
  }

  bool DictionaryStore::contains(std::string_view str) const
  {
    auto const lock = std::shared_lock{_mutex};
    return _stringToId.contains(str);
  }

  std::uint64_t DictionaryStore::generation() const
  {
    auto const lock = std::shared_lock{_mutex};
    return _generation;
  }

  std::uint64_t DictionaryStore::bindSymbols(std::span<std::string const> symbols, std::span<DictionaryId> ids) const
  {
    gsl_Expects(symbols.size() == ids.size());
    auto const lock = std::shared_lock{_mutex};

    for (std::size_t index = 0; index < symbols.size(); ++index)
    {
      if (auto const it = _stringToId.find(symbols[index]); it != _stringToId.end())
      {
        ids[index] = *it;
      }
      else
      {
        ids[index] = kInvalidDictionaryId;
      }
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
      auto const committedSize = dictionary->_idToStringStorage.size();
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
      if (!transaction->isActive())
      {
        return makeError(Error::Code::InvalidState, "Dictionary writer transaction is no longer active");
      }

      if (auto const optId = dictionary->findId(value); optId)
      {
        return *optId;
      }

      if (auto const it = overlay.find(value); it != overlay.end())
      {
        return it->second;
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
      auto const [overlayIt, inserted] = overlay.emplace(std::string{value}, id);
      gsl_Expects(inserted);

      try
      {
        delta.push_back(Delta{.id = id, .text = overlayIt->first});
      }
      catch (...)
      {
        overlay.erase(overlayIt);
        throw;
      }

      if (auto result = optWriter->create(id.raw(), utility::bytes::view(delta.back().text)); !result)
      {
        delta.pop_back();
        overlay.erase(overlayIt);
        return std::unexpected{result.error()};
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

      publicationLock = std::unique_lock{dictionary->_mutex};

      auto const expectedFirstId = static_cast<std::uint32_t>(dictionary->_idToStringStorage.size()) + 1;

      if (delta.front().id.raw() != expectedFirstId)
      {
        throwException<Exception>("Dictionary publication order changed during a library write");
      }

      try
      {
        dictionary->_stringToId.reserve(dictionary->_stringToId.size() + delta.size());

        for (auto& entry : delta)
        {
          dictionary->_idToStringStorage.emplace_back(std::move(entry.text));
          appendedCount++;
        }

        for (auto const& entry : delta)
        {
          auto const [it, inserted] = dictionary->_stringToId.insert(entry.id);

          if (!inserted)
          {
            throwException<Exception>("Dictionary publication would rebind an existing string");
          }

          insertedCount++;
        }
      }
      catch (...)
      {
        rollbackPublication();
        throw;
      }
    }

    void publish() noexcept
    {
      if (!publicationLock.owns_lock())
      {
        return;
      }

      dictionary->_generation++;
      insertedCount = 0;
      appendedCount = 0;
      publicationLock.unlock();
    }

    void rollbackPublication() noexcept
    {
      if (!publicationLock.owns_lock())
      {
        return;
      }

      for (std::size_t index = 0; index < insertedCount; ++index)
      {
        dictionary->_stringToId.erase(delta[index].id);
      }

      while (appendedCount > 0)
      {
        dictionary->_idToStringStorage.pop_back();
        appendedCount--;
      }

      insertedCount = 0;
      publicationLock.unlock();
    }

    DictionaryStore* dictionary;
    lmdb::WriteTransaction* transaction;
    std::optional<lmdb::Database::Writer> optWriter;
    std::map<std::string, DictionaryId, std::less<>> overlay;
    std::vector<Delta> delta;
    std::unique_lock<std::shared_mutex> publicationLock;
    std::uint32_t nextId = 1;
    std::size_t appendedCount = 0;
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

  std::optional<DictionaryId> DictionaryReadContext::findId(std::string_view text) const
  {
    return _dictionary->findId(text);
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

  std::uint64_t DictionaryReadContext::generation() const
  {
    return _dictionary->generation();
  }
} // namespace ao::library
