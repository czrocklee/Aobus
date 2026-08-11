// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/library/TrackStore.h>

#include "lmdb/detail/TransactionFailure.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/TrackView.h>
#include <ao/library/WriteTransaction.h>
#include <ao/lmdb/Database.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace ao::library
{
  namespace
  {
    void validateLoadedTrack(TrackView const& view, TrackStore::Reader::LoadMode const mode)
    {
      if (mode == TrackStore::Reader::LoadMode::Hot || mode == TrackStore::Reader::LoadMode::Both)
      {
        AO_INVARIANT(view.isHotValid(), "Hot Track record is structurally corrupt after library validation");
      }

      if (mode == TrackStore::Reader::LoadMode::Cold || mode == TrackStore::Reader::LoadMode::Both)
      {
        AO_INVARIANT(view.isColdValid(), "Cold Track record is structurally corrupt after library validation");
      }
    }
  } // namespace

  // TrackStore implementation
  TrackStore::TrackStore(lmdb::IntegerKeyDatabase hotDb,
                         lmdb::IntegerKeyDatabase coldDb,
                         detail::LibraryIdentity const& identity)
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

  TrackStore::Reader TrackStore::reader(LibraryWrite const& write) const
  {
    auto const& native = write.native(*_identity);
    return Reader{_hotDb.reader(native), _coldDb.reader(native)};
  }

  TrackStore::Writer TrackStore::writer(WriteTransaction& transaction) const
  {
    auto& native = transaction.native(*_identity);
    return Writer{_hotDb.writer(native), _coldDb.writer(native)};
  }

  // TrackStore::Reader implementation
  TrackStore::Reader::Reader(lmdb::IntegerKeyDatabase::Reader hotReader, lmdb::IntegerKeyDatabase::Reader coldReader)
    : _hotReader{std::move(hotReader)}, _coldReader{std::move(coldReader)}
  {
  }

  std::optional<TrackView> TrackStore::Reader::get(TrackId id, LoadMode mode) const
  {
    auto optHotBytes = std::optional<std::span<std::byte const>>{};
    auto optColdBytes = std::optional<std::span<std::byte const>>{};
    auto hotBuffer = std::span<std::byte const>{};
    auto coldBuffer = std::span<std::byte const>{};

    if (mode == LoadMode::Hot || mode == LoadMode::Both)
    {
      optHotBytes = _hotReader.get(id.raw());
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      optColdBytes = _coldReader.get(id.raw());
    }

    if (mode == LoadMode::Both)
    {
      AO_INVARIANT(optHotBytes.has_value() == optColdBytes.has_value(),
                   "Track hot/cold presence diverged after library validation");
    }

    if ((mode != LoadMode::Cold && !optHotBytes) || (mode != LoadMode::Hot && !optColdBytes))
    {
      return std::nullopt;
    }

    hotBuffer = optHotBytes.value_or(hotBuffer);
    coldBuffer = optColdBytes.value_or(coldBuffer);
    auto view = TrackView{hotBuffer, coldBuffer};
    validateLoadedTrack(view, mode);
    return view;
  }

  bool TrackStore::Reader::shouldUseCursorScan(std::span<TrackId const> ids, LoadMode const mode) const
  {
    if (ids.empty())
    {
      return false;
    }

    auto const rowCount = [&]
    {
      switch (mode)
      {
        case LoadMode::Hot: return _hotReader.entryCount();
        case LoadMode::Cold: return _coldReader.entryCount();
        case LoadMode::Both: return entryCount();
      }

      AO_FATAL("Unknown Track load mode");
    }();

    constexpr std::size_t kCursorScanDensityDenominator = 4;
    auto const minimumDenseSelection = (rowCount / kCursorScanDensityDenominator) +
                                       static_cast<std::size_t>(rowCount % kCursorScanDensityDenominator != 0);

    if (rowCount == 0 || ids.size() < minimumDenseSelection)
    {
      return false;
    }

    return std::ranges::adjacent_find(ids, std::ranges::greater_equal{}) == ids.end();
  }

  std::size_t TrackStore::Reader::entryCount() const
  {
    auto const hotCount = _hotReader.entryCount();
    auto const coldCount = _coldReader.entryCount();
    AO_INVARIANT(hotCount == coldCount, "Track hot/cold row counts diverged after library validation");
    return hotCount;
  }

  TrackStore::Reader::Iterator TrackStore::Reader::begin() const
  {
    return beginFor(LoadMode::Both);
  }

  TrackStore::Reader::Iterator TrackStore::Reader::beginFor(LoadMode mode) const
  {
    using DatabaseIterator = lmdb::IntegerKeyDatabase::Reader::Iterator;

    switch (mode)
    {
      case LoadMode::Hot: return Iterator{_hotReader.begin(), DatabaseIterator{}, mode};
      case LoadMode::Cold: return Iterator{DatabaseIterator{}, _coldReader.begin(), mode};
      case LoadMode::Both: return Iterator{_hotReader.begin(), _coldReader.begin(), mode};
    }

    AO_FATAL("Unknown Track load mode");
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::IntegerKeyDatabase::Reader::Iterator&& hotIter,
                                         lmdb::IntegerKeyDatabase::Reader::Iterator&& coldIter,
                                         Reader::LoadMode mode)
    : _hotIter{std::move(hotIter)}, _coldIter{std::move(coldIter)}, _mode{mode}
  {
    if (_mode == LoadMode::Both)
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

    auto const end = lmdb::IntegerKeyDatabase::Reader::Iterator{};
    auto const hotAtEnd = _hotIter == end;
    auto const coldAtEnd = _coldIter == end;
    AO_INVARIANT(hotAtEnd == coldAtEnd);

    if (!hotAtEnd)
    {
      auto const hotId = static_cast<std::uint32_t>((*_hotIter).first);
      auto const coldId = static_cast<std::uint32_t>((*_coldIter).first);
      AO_INVARIANT(hotId == coldId);
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
    auto const end = lmdb::IntegerKeyDatabase::Reader::Iterator{};

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
      trackId = TrackId{static_cast<std::uint32_t>(item.first)};
      hotBuffer = item.second;
    }

    if (_mode != LoadMode::Hot)
    {
      auto const item = *_coldIter;

      if (_mode == LoadMode::Cold)
      {
        trackId = TrackId{static_cast<std::uint32_t>(item.first)};
      }

      coldBuffer = item.second;
    }

    auto view = TrackView{hotBuffer, coldBuffer};
    validateLoadedTrack(view, _mode);

    return {trackId, view};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::IntegerKeyDatabase::Writer hotWriter, lmdb::IntegerKeyDatabase::Writer coldWriter)
    : _hotWriter{std::move(hotWriter)}, _coldWriter{std::move(coldWriter)}
  {
  }

  std::optional<TrackView> TrackStore::Writer::get(TrackId id, Reader::LoadMode mode) const
  {
    auto optHotBytes = std::optional<std::span<std::byte const>>{};
    auto optColdBytes = std::optional<std::span<std::byte const>>{};
    auto hotBuffer = std::span<std::byte const>{};
    auto coldBuffer = std::span<std::byte const>{};

    if (mode == Reader::LoadMode::Hot || mode == Reader::LoadMode::Both)
    {
      optHotBytes = _hotWriter.get(id.raw());
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      optColdBytes = _coldWriter.get(id.raw());
    }

    if (mode == Reader::LoadMode::Both)
    {
      AO_INVARIANT(optHotBytes.has_value() == optColdBytes.has_value(),
                   "Track hot/cold presence diverged after library validation");
    }

    if ((mode != Reader::LoadMode::Cold && !optHotBytes) || (mode != Reader::LoadMode::Hot && !optColdBytes))
    {
      return std::nullopt;
    }

    hotBuffer = optHotBytes.value_or(hotBuffer);
    coldBuffer = optColdBytes.value_or(coldBuffer);
    auto view = TrackView{hotBuffer, coldBuffer};
    validateLoadedTrack(view, mode);
    return view;
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
      lmdb::detail::throwTransactionFailure(std::move(result.error()));
    }

    if (auto result = _coldWriter.clear(); !result)
    {
      lmdb::detail::throwTransactionFailure(std::move(result.error()));
    }

    return {};
  }
} // namespace ao::library
