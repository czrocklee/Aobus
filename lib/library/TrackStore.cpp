// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/library/TrackStore.h>

#include <gsl-lite/gsl-lite.hpp>

namespace rs::library
{
  // TrackStore implementation
  TrackStore::TrackStore(lmdb::Database hotDb, lmdb::Database coldDb)
    : _hotDb{std::move(hotDb)}, _coldDb{std::move(coldDb)}
  {
  }

  TrackStore::Reader TrackStore::reader(lmdb::ReadTransaction& txn) const
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
      auto optHotBuffer = _hotReader.get(id.value());

      if (!optHotBuffer)
      {
        return std::nullopt;
      }

      hotBuffer = *optHotBuffer;
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      auto optColdBuffer = _coldReader.get(id.value());

      if (!optColdBuffer)
      {
        return std::nullopt;
      }

      coldBuffer = *optColdBuffer;
    }

    return TrackView{hotBuffer, coldBuffer};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::begin(LoadMode mode) const
  {
    return Iterator{_hotReader.begin(), _coldReader.begin(), mode};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::end(LoadMode mode) const
  {
    return Iterator{_hotReader.end(), _coldReader.end(), mode};
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& hotIter,
                                         lmdb::Database::Reader::Iterator&& coldIter,
                                         Reader::LoadMode mode)
    : _hotIter{mode != LoadMode::Cold ? std::make_optional(std::move(hotIter)) : std::nullopt}
    , _coldIter{mode != LoadMode::Hot ? std::make_optional(std::move(coldIter)) : std::nullopt}
    , _mode{mode}
  {
  }

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    if (_mode != other._mode)
    {
      return false;
    }

    switch (_mode)
    {
      case LoadMode::Cold: return _coldIter == other._coldIter;
      case LoadMode::Hot:
      case LoadMode::Both: return _hotIter == other._hotIter;
    }

    return false;
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    if (_hotIter)
    {
      ++(*_hotIter);
    }

    if (_coldIter)
    {
      ++(*_coldIter);
    }

    return *this;
  }

  TrackStore::Reader::Iterator::value_type TrackStore::Reader::Iterator::operator*() const
  {
    TrackId trackId;

    std::span<std::byte const> hotData;
    std::span<std::byte const> coldData;

    if (_hotIter)
    {
      auto&& [id, hotBuffer] = **_hotIter;
      trackId = TrackId{id};
      hotData = hotBuffer;
    }

    if (_coldIter)
    {
      auto&& [coldId, coldBuffer] = **_coldIter;

      if (!_hotIter)
      {
        trackId = TrackId{coldId};
      }
      else
      {
        gsl_Expects(coldId == trackId.value());
      }

      coldData = coldBuffer;
    }

    return {trackId, TrackView{hotData, coldData}};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter)
    : _hotWriter{std::move(hotWriter)}, _coldWriter{std::move(coldWriter)}
  {
  }

  // Hot/Cold split methods
  std::pair<TrackId, TrackView> TrackStore::Writer::createHotCold(std::span<std::byte const> hotData,
                                                                  std::span<std::byte const> coldData)
  {
    gsl_Expects((hotData.size() % 4) == 0);
    gsl_Expects((coldData.size() % 4) == 0);

    auto id = _hotWriter.append(hotData);
    auto coldId = _coldWriter.append(coldData);
    gsl_Ensures(id == coldId);
    return {TrackId{id}, TrackView{hotData, coldData}};
  }

  void TrackStore::Writer::updateHot(TrackId id, std::span<std::byte const> hotData)
  {
    gsl_Expects((hotData.size() % 4) == 0);

    _hotWriter.update(id.value(), hotData);
  }

  void TrackStore::Writer::updateCold(TrackId id, std::span<std::byte const> coldData)
  {
    gsl_Expects((coldData.size() % 4) == 0);

    _coldWriter.update(id.value(), coldData);
  }

  bool TrackStore::Writer::remove(TrackId id)
  {
    bool hotDeleted = _hotWriter.del(id.value());
    bool coldDeleted = _coldWriter.del(id.value());
    return hotDeleted && coldDeleted;
  }

  void TrackStore::Writer::clear()
  {
    _hotWriter.clear();
    _coldWriter.clear();
  }

  std::optional<TrackView> TrackStore::Writer::get(TrackId id, Reader::LoadMode mode) const
  {
    if (mode == Reader::LoadMode::Hot)
    {
      return _hotWriter.get(id.value()).transform([](auto const& buffer) { return TrackView{buffer, {}}; });
    }

    if (mode == Reader::LoadMode::Cold)
    {
      return _coldWriter.get(id.value()).transform([](auto const& buffer) { return TrackView{{}, buffer}; });
    }

    // Both
    return _hotWriter.get(id.value())
      .and_then(
        [this, id](auto const& hotBuffer)
        {
          return _coldWriter.get(id.value())
            .transform([&hotBuffer](auto const& coldBuffer) { return TrackView{hotBuffer, coldBuffer}; });
        });
  }
} // namespace rs::library
