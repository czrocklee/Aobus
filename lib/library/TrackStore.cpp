// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackStore.h>

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/TrackView.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/TransactionFailure.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace ao::library
{
  // TrackStore implementation
  TrackStore::TrackStore(lmdb::Database hotDb, lmdb::Database coldDb, detail::LibraryIdentity const& identity)
    : _hotDb{std::move(hotDb)}, _coldDb{std::move(coldDb)}, _identity{&identity}
  {
  }

  TrackStore::Reader TrackStore::reader(ReadTransaction const& transaction) const
  {
    auto const& native = transaction.native(*_identity);
    return Reader{_hotDb.reader(native), _coldDb.reader(native)};
  }

  TrackStore::Reader TrackStore::reader(WriteTransaction const& transaction) const
  {
    auto const& native = transaction.native(*_identity);
    return Reader{_hotDb.reader(native), _coldDb.reader(native)};
  }

  TrackStore::Writer TrackStore::writer(WriteTransaction& transaction) const
  {
    auto& native = transaction.native(*_identity);
    return Writer{_hotDb.writer(native), _coldDb.writer(native)};
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

  bool TrackStore::Reader::shouldUseCursorScan(std::span<TrackId const> ids, LoadMode mode) const
  {
    if (ids.empty())
    {
      return false;
    }

    auto const rowCount = entryCount(mode);

    constexpr std::size_t kCursorScanDensityDenominator = 4;
    auto const minimumDenseSelection = (rowCount / kCursorScanDensityDenominator) +
                                       static_cast<std::size_t>(rowCount % kCursorScanDensityDenominator != 0);

    if (rowCount == 0 || ids.size() < minimumDenseSelection)
    {
      return false;
    }

    return std::ranges::adjacent_find(ids, std::ranges::greater_equal{}) == ids.end();
  }

  std::size_t TrackStore::Reader::entryCount(LoadMode const mode) const
  {
    switch (mode)
    {
      case LoadMode::Hot: return _hotReader.entryCount();
      case LoadMode::Cold: return _coldReader.entryCount();
      case LoadMode::Both:
      {
        auto const hotCount = _hotReader.entryCount();
        auto const coldCount = _coldReader.entryCount();
        gsl_Expects(hotCount == coldCount);
        return hotCount;
      }
    }

    return 0;
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
    : _hotIter{std::move(hotIter)}, _coldIter{std::move(coldIter)}, _mode{mode}
  {
    if (_mode == LoadMode::Hot)
    {
      _coldIter = lmdb::Database::Reader::Iterator{};
    }
    else if (_mode == LoadMode::Cold)
    {
      _hotIter = lmdb::Database::Reader::Iterator{};
    }
    else
    {
      validateBothPosition();
    }
  }

  void TrackStore::Reader::Iterator::validateBothPosition() const
  {
    if (_mode != LoadMode::Both)
    {
      return;
    }

    auto const end = lmdb::Database::Reader::Iterator{};
    auto const hotAtEnd = _hotIter == end;
    auto const coldAtEnd = _coldIter == end;
    gsl_Expects(hotAtEnd == coldAtEnd);

    if (!hotAtEnd)
    {
      auto const hotId = static_cast<std::uint32_t>((*_hotIter).first);
      auto const coldId = static_cast<std::uint32_t>((*_coldIter).first);
      gsl_Expects(hotId == coldId);
    }
  }

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    if (_mode != other._mode)
    {
      return false;
    }

    if (_mode == LoadMode::Hot)
    {
      return _hotIter == other._hotIter;
    }

    if (_mode == LoadMode::Cold)
    {
      return _coldIter == other._coldIter;
    }

    return _hotIter == other._hotIter && _coldIter == other._coldIter;
  }

  bool TrackStore::Reader::Iterator::operator==(EndSentinel /*unused*/) const
  {
    auto const end = lmdb::Database::Reader::Iterator{};

    if (_mode == LoadMode::Hot)
    {
      return _hotIter == end;
    }

    if (_mode == LoadMode::Cold)
    {
      return _coldIter == end;
    }

    validateBothPosition();
    return _hotIter == end;
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    if (_mode == LoadMode::Both)
    {
      ++_hotIter;
      ++_coldIter;
      validateBothPosition();
      return *this;
    }

    if (_mode == LoadMode::Hot)
    {
      ++_hotIter;
    }
    else
    {
      ++_coldIter;
    }

    return *this;
  }

  TrackStore::Reader::Iterator::value_type TrackStore::Reader::Iterator::operator*() const
  {
    validateBothPosition();

    auto trackId = TrackId{};
    auto hotBuffer = std::span<std::byte const>{};
    auto coldBuffer = std::span<std::byte const>{};

    if (_mode != LoadMode::Cold)
    {
      auto const item = *_hotIter;
      trackId = TrackId{item.first};
      hotBuffer = item.second;
    }

    if (_mode != LoadMode::Hot)
    {
      auto const item = *_coldIter;

      if (_mode == LoadMode::Cold)
      {
        trackId = TrackId{item.first};
      }

      coldBuffer = item.second;
    }

    auto view = TrackView{hotBuffer, coldBuffer};

    return {trackId, view};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer hotWriter, lmdb::Database::Writer coldWriter)
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

  bool TrackStore::Writer::remove(TrackId id)
  {
    auto const hotRemoved = _hotWriter.del(id.raw());
    auto const coldRemoved = _coldWriter.del(id.raw());
    return hotRemoved || coldRemoved;
  }

  Result<> TrackStore::Writer::clear()
  {
    if (auto result = _hotWriter.clear(); !result)
    {
      lmdb::throwTransactionFailure(std::move(result.error()));
    }

    if (auto result = _coldWriter.clear(); !result)
    {
      lmdb::throwTransactionFailure(std::move(result.error()));
    }

    return {};
  }
} // namespace ao::library
