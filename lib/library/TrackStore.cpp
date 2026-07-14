// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Transaction.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
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

  TrackStore::Reader TrackStore::reader(lmdb::ReadTransaction const& transaction) const
  {
    return Reader{_hotDb.reader(transaction), _coldDb.reader(transaction)};
  }

  TrackStore::Writer TrackStore::writer(lmdb::WriteTransaction& transaction)
  {
    return Writer{_hotDb.writer(transaction), _coldDb.writer(transaction)};
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

    std::size_t rowCount = 0;

    switch (mode)
    {
      case LoadMode::Hot: rowCount = _hotReader.entryCount(); break;
      case LoadMode::Cold: rowCount = _coldReader.entryCount(); break;
      case LoadMode::Both: rowCount = std::min(_hotReader.entryCount(), _coldReader.entryCount()); break;
    }

    constexpr std::size_t kCursorScanDensityDenominator = 4;
    auto const minimumDenseSelection = (rowCount / kCursorScanDensityDenominator) +
                                       static_cast<std::size_t>(rowCount % kCursorScanDensityDenominator != 0);

    if (rowCount == 0 || ids.size() < minimumDenseSelection)
    {
      return false;
    }

    return std::ranges::adjacent_find(ids, std::ranges::greater_equal{}) == ids.end();
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
      alignBoth();
    }
  }

  void TrackStore::Reader::Iterator::alignBoth()
  {
    auto const end = lmdb::Database::Reader::Iterator{};

    while (_hotIter != end && _coldIter != end)
    {
      auto const hotId = static_cast<std::uint32_t>((*_hotIter).first);
      auto const coldId = static_cast<std::uint32_t>((*_coldIter).first);

      if (hotId == coldId)
      {
        return;
      }

      if (hotId < coldId)
      {
        ++_hotIter;
      }
      else
      {
        ++_coldIter;
      }
    }

    _hotIter = lmdb::Database::Reader::Iterator{};
    _coldIter = lmdb::Database::Reader::Iterator{};
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

    return _hotIter == end && _coldIter == end;
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    if (_mode == LoadMode::Both)
    {
      ++_hotIter;
      ++_coldIter;
      alignBoth();
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
    if (auto validation = detail::validateSerializedTrackBytes(hotData, "hot"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    if (auto validation = detail::validateSerializedTrackBytes(coldData, "cold"); !validation)
    {
      return std::unexpected{validation.error()};
    }

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
    if (auto validation = detail::validateSerializedTrackBytes(hotData, "hot"); !validation)
    {
      return validation;
    }

    return _hotWriter.update(id.raw(), hotData);
  }

  Result<std::span<std::byte>> TrackStore::Writer::updateCold(TrackId id, std::size_t size)
  {
    if (auto validation = detail::validateSerializedTrackSize(size, "cold"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    auto spanResult = _coldWriter.update(id.raw(), size);

    if (!spanResult)
    {
      return spanResult;
    }

    if (auto validation = detail::validateSerializedTrackBytes(*spanResult, "cold"); !validation)
    {
      return std::unexpected{validation.error()};
    }

    return spanResult;
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
