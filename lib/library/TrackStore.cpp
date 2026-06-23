// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/Type.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace ao::library
{
  // TrackStore implementation
  TrackStore::TrackStore(lmdb::Database hotDb, lmdb::Database coldDb)
    : _hotDb{std::move(hotDb)}, _coldDb{std::move(coldDb)}
  {
  }

  TrackStore::Reader TrackStore::reader(lmdb::ReadTransaction const& txn) const
  {
    return Reader{_hotDb.reader(txn), _coldDb.reader(txn)};
  }

  TrackStore::Writer TrackStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_hotDb.writer(txn), _coldDb.writer(txn)};
  }

  // TrackStore::Reader implementation
  TrackStore::Reader::Reader(lmdb::Database::Reader hotReader, lmdb::Database::Reader coldReader)
    : _hotReader{std::move(hotReader)}, _coldReader{std::move(coldReader)}
  {
  }

  std::optional<TrackView> TrackStore::Reader::get(TrackId id, LoadMode mode) const
  {
    auto hotBuffer = std::span<std::byte const>{};
    auto coldBuffer = std::span<std::byte const>{};

    if (mode == LoadMode::Hot || mode == LoadMode::Both)
    {
      auto optHotBytes = _hotReader.get(id.raw());

      if (!optHotBytes)
      {
        return std::nullopt;
      }

      hotBuffer = *optHotBytes;
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      auto optColdBytes = _coldReader.get(id.raw());

      if (!optColdBytes)
      {
        return std::nullopt;
      }

      coldBuffer = *optColdBytes;
    }

    return TrackView{hotBuffer, coldBuffer};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::begin(LoadMode mode) const
  {
    return Iterator{_hotReader.begin(), _coldReader.begin(), mode};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::end(LoadMode mode) const
  {
    return Iterator{lmdb::Database::Reader::Iterator{}, lmdb::Database::Reader::Iterator{}, mode};
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& hotIter,
                                         lmdb::Database::Reader::Iterator&& coldIter,
                                         Reader::LoadMode mode)
    : _optHotIter{mode != LoadMode::Cold ? std::make_optional(std::move(hotIter)) : std::nullopt}
    , _optColdIter{mode != LoadMode::Hot ? std::make_optional(std::move(coldIter)) : std::nullopt}
    , _mode{mode}
  {
  }

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    if (_mode != other._mode)
    {
      return false;
    }

    if (_optHotIter && other._optHotIter && *(_optHotIter) != *(other._optHotIter))
    {
      return false;
    }

    if (_optColdIter && other._optColdIter && *(_optColdIter) != *(other._optColdIter))
    {
      return false;
    }

    return true;
  }

  bool TrackStore::Reader::Iterator::operator==(EndSentinel /*unused*/) const
  {
    auto isAtEnd = [](std::optional<lmdb::Database::Reader::Iterator> const& opt) -> bool
    { return !opt || *opt == lmdb::Database::Reader::Iterator{}; };
    return isAtEnd(_optHotIter) && isAtEnd(_optColdIter);
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    if (_optHotIter)
    {
      ++(*_optHotIter);
    }

    if (_optColdIter)
    {
      ++(*_optColdIter);
    }

    return *this;
  }

  TrackStore::Reader::Iterator::value_type TrackStore::Reader::Iterator::operator*() const
  {
    auto trackId = TrackId{};
    auto hotBuffer = std::span<std::byte const>{};
    auto coldBuffer = std::span<std::byte const>{};

    if (_optHotIter)
    {
      auto const item = **_optHotIter;
      trackId = TrackId{item.first};
      hotBuffer = item.second;
    }

    if (_optColdIter)
    {
      auto const item = **_optColdIter;
      trackId = TrackId{item.first};
      coldBuffer = item.second;
    }

    auto view = TrackView{hotBuffer, coldBuffer};

    return {trackId, view};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter)
    : _hotWriter{std::move(hotWriter)}, _coldWriter{std::move(coldWriter)}
  {
  }

  std::optional<TrackView> TrackStore::Writer::get(TrackId id, Reader::LoadMode mode) const
  {
    auto hotBuffer = std::span<std::byte const>{};
    auto coldBuffer = std::span<std::byte const>{};

    if (mode == Reader::LoadMode::Hot || mode == Reader::LoadMode::Both)
    {
      auto optHotBytes = _hotWriter.get(id.raw());

      if (!optHotBytes)
      {
        return std::nullopt;
      }

      hotBuffer = *optHotBytes;
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      auto optColdBytes = _coldWriter.get(id.raw());

      if (!optColdBytes)
      {
        return std::nullopt;
      }

      coldBuffer = *optColdBytes;
    }

    return TrackView{hotBuffer, coldBuffer};
  }

  // Hot/Cold split methods
  Result<std::pair<TrackId, TrackView>> TrackStore::Writer::createHotCold(std::span<std::byte const> hotData,
                                                                          std::span<std::byte const> coldData)
  {
    gsl_Expects((hotData.size() % 4) == 0);
    gsl_Expects((coldData.size() % 4) == 0);

    auto idResult = _hotWriter.append(hotData);

    if (!idResult)
    {
      return std::unexpected{idResult.error()};
    }

    auto id = *idResult;

    if (auto result = _coldWriter.create(id, coldData); !result)
    {
      return std::unexpected{result.error()};
    }

    return std::pair{TrackId{id}, TrackView{hotData, coldData}};
  }

  Result<> TrackStore::Writer::updateHot(TrackId id, std::span<std::byte const> hotData)
  {
    gsl_Expects((hotData.size() % 4) == 0);

    return _hotWriter.update(id.raw(), hotData);
  }

  Result<std::span<std::byte>> TrackStore::Writer::updateCold(TrackId id, std::size_t size)
  {
    gsl_Expects((size % 4) == 0);
    return _coldWriter.update(id.raw(), size);
  }

  bool TrackStore::Writer::remove(TrackId id)
  {
    bool const hotRemoved = _hotWriter.del(id.raw());
    bool const coldRemoved = _coldWriter.del(id.raw());

    return hotRemoved || coldRemoved;
  }

  Result<> TrackStore::Writer::clear()
  {
    if (auto result = _hotWriter.clear(); !result)
    {
      return result;
    }

    return _coldWriter.clear();
  }
} // namespace ao::library
