// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackStore.h>

#include <cassert>

namespace rs::core
{

  // TrackStore implementation
  TrackStore::TrackStore(lmdb::WriteTransaction& txn, std::string const& hotDb, std::string const& coldDb)
    : _hotDb{txn, hotDb}
    , _coldDb{txn, coldDb}
  {
  }

  TrackStore::Reader TrackStore::reader(lmdb::ReadTransaction& txn) const
  {
    return Reader{_hotDb.reader(txn), _coldDb.reader(txn), Reader::LoadMode::Both};
  }

  TrackStore::Writer TrackStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_hotDb.writer(txn), _coldDb.writer(txn)};
  }

  // TrackStore::Reader implementation
  TrackStore::Reader::Reader(lmdb::Database::Reader&& hotReader, lmdb::Database::Reader&& coldReader, LoadMode mode)
    : _hotReader{std::move(hotReader)}
    , _coldReader{std::move(coldReader)}
    , _mode{mode}
  {
  }

  std::optional<TrackView> TrackStore::Reader::get(TrackId id, LoadMode mode) const
  {
    std::span<std::byte const> hotBuffer;
    std::span<std::byte const> coldBuffer;

    if (mode == LoadMode::Hot || mode == LoadMode::Both)
    {
      auto optHotBuffer = _hotReader.get(id.value());
      if (!optHotBuffer || optHotBuffer->empty()) { return std::nullopt; }
      hotBuffer = *optHotBuffer;
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      auto optColdBuffer = _coldReader.get(id.value());
      if (!optColdBuffer || optColdBuffer->empty()) { return std::nullopt; }
      coldBuffer = *optColdBuffer;
    }

    return TrackView{hotBuffer, coldBuffer};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::begin(LoadMode mode) const
  {
    return Iterator{_hotReader.begin(), _coldReader.begin(), mode};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::end() const
  {
    return Iterator{_hotReader.end(), _coldReader.end(), _mode};
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& hotIter,
                                         lmdb::Database::Reader::Iterator&& coldIter,
                                         Reader::LoadMode mode)
    : _hotIter{mode != LoadMode::Cold ? std::make_optional(std::move(hotIter)) : std::nullopt}
    , _coldIter{mode != LoadMode::Hot ? std::make_optional(std::move(coldIter)) : std::nullopt}
  {
  }

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _hotIter == other._hotIter;
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    if (_hotIter) { ++(*_hotIter); }
    if (_coldIter) { ++(*_coldIter); }
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
      assert(coldId == trackId.value() && "cold and hot must have same track ID");
      coldData = coldBuffer;
    }

    return {trackId, TrackView{hotData, coldData}};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter)
    : _hotWriter{std::move(hotWriter)}
    , _coldWriter{std::move(coldWriter)}
  {
  }

  // Hot/Cold split methods
  std::pair<TrackId, TrackView> TrackStore::Writer::createHotCold(
      std::span<std::byte const> hotData,
      std::span<std::byte const> coldData)
  {
    // Ensure size is multiple of 4 for LMDB
    assert((hotData.size() % 4 == 0) && "hotData size must be multiple of 4");
    assert((coldData.size() % 4 == 0) && "coldData size must be multiple of 4");

    auto id = _hotWriter.append(hotData);
    [[maybe_unused]] auto coldId = _coldWriter.append(coldData);
    return {TrackId{id}, TrackView{hotData, coldData}};
  }

  TrackView TrackStore::Writer::updateHot(TrackId id, std::span<std::byte const> hotData)
  {
    // Ensure size is multiple of 4 for LMDB
    assert((hotData.size() % 4 == 0) && "hotData size must be multiple of 4");

    [[maybe_unused]] auto buffer = _hotWriter.update(id.value(), hotData);
    return TrackView{hotData, {}};
  }

  TrackView TrackStore::Writer::updateCold(TrackId id, std::span<std::byte const> coldData)
  {
    // Ensure size is multiple of 4 for LMDB
    assert((coldData.size() % 4 == 0) && "coldData size must be multiple of 4");

    [[maybe_unused]] auto buffer = _coldWriter.update(id.value(), coldData);
    return TrackView{{}, coldData};
  }

  bool TrackStore::Writer::remove(TrackId id)
  {
    bool hotDeleted = _hotWriter.del(id.value());
    bool coldDeleted = _coldWriter.del(id.value());
    return hotDeleted && coldDeleted;
  }

  std::optional<TrackView> TrackStore::Writer::get(TrackId id, Reader::LoadMode mode) const
  {
    std::span<std::byte const> hotBuffer;
    std::span<std::byte const> coldBuffer;

    if (mode == Reader::LoadMode::Hot || mode == Reader::LoadMode::Both)
    {
      auto optBuffer = _hotWriter.get(id.value());
      if (!optBuffer || optBuffer->empty()) { return std::nullopt; }
      hotBuffer = *optBuffer;
    }

    if (mode == Reader::LoadMode::Cold || mode == Reader::LoadMode::Both)
    {
      auto optColdBuffer = _coldWriter.get(id.value());
      if (!optColdBuffer || optColdBuffer->empty()) { return std::nullopt; }
      coldBuffer = *optColdBuffer;
    }

    return TrackView{hotBuffer, coldBuffer};
  }

} // namespace rs::core
