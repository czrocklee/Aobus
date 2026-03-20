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
    return Reader{_hotDb.reader(txn), _coldDb.reader(txn)};
  }

  TrackStore::Writer TrackStore::writer(lmdb::WriteTransaction& txn)
  {
    return Writer{_hotDb.writer(txn), _coldDb.writer(txn)};
  }

  // TrackStore::Reader implementation
  TrackStore::Reader::Reader(lmdb::Database::Reader&& hotReader, lmdb::Database::Reader&& coldReader)
    : _hotReader{std::move(hotReader)}
    , _coldReader{std::move(coldReader)}
    , _coldLoader{[this](TrackId id) -> std::optional<std::span<std::byte const>> {
      auto optBuffer = _coldReader.get(id.value());
      if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
      return optBuffer;
    }}
  {
  }

  std::optional<TrackView> TrackStore::Reader::get(TrackId id) const
  {
    auto optHotBuffer = _hotReader.get(id.value());
    if (!optHotBuffer || optHotBuffer->size() == 0) { return std::nullopt; }

    std::optional<std::span<std::byte const>> optColdBuffer = _coldReader.get(id.value());
    if (optColdBuffer && optColdBuffer->size() == 0) {
      optColdBuffer = std::nullopt;
    }

    return TrackView{id, *optHotBuffer, optColdBuffer, _coldLoader};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::begin(ColdLoadHint hint) const
  {
    if (hint == ColdLoadHint::Eager) {
      return Iterator{_hotReader.begin(), _coldReader.begin(), hint, _coldLoader};
    }
    std::optional<lmdb::Database::Reader::Iterator> coldIter;
    return Iterator{_hotReader.begin(), coldIter, hint, _coldLoader};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::beginEager() const
  {
    return Iterator{_hotReader.begin(), std::optional{_coldReader.begin()}, ColdLoadHint::Eager, _coldLoader};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::end() const
  {
    return Iterator{_hotReader.end(), std::nullopt, ColdLoadHint::Lazy, _coldLoader};
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& hotIter,
                                         std::optional<lmdb::Database::Reader::Iterator> coldIter,
                                         ColdLoadHint hint,
                                         std::function<std::optional<std::span<std::byte const>>(TrackId)> coldLoader)
    : _hotIter{std::move(hotIter)}
    , _coldIter{std::move(coldIter)}
    , _hint{hint}
    , _coldLoader{std::move(coldLoader)}
  {
  }

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _hotIter == other._hotIter;
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    ++_hotIter;
    if (_coldIter) {
      ++(*_coldIter);
    }
    return *this;
  }

  TrackStore::Reader::Iterator::value_type TrackStore::Reader::Iterator::operator*() const
  {
    auto&& [id, hotBuffer] = *_hotIter;
    auto trackId = TrackId{id};

    std::optional<std::span<std::byte const>> coldBuffer;
    if (_hint == ColdLoadHint::Eager && _coldIter) {
      auto&& [coldId, coldBuf] = **_coldIter;
      coldBuffer = coldBuf;
    }

    return {trackId, TrackView{trackId, hotBuffer, coldBuffer, _coldLoader}};
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

    auto [id, hotBuffer] = _hotWriter.append(hotData);
    [[maybe_unused]] auto [coldId, coldBuffer] = _coldWriter.append(coldData);
    return {TrackId{id}, TrackView{TrackId{id}, hotData, coldData, nullptr}};
  }

  TrackView TrackStore::Writer::updateHot(TrackId id, std::span<std::byte const> hotData)
  {
    // Ensure size is multiple of 4 for LMDB
    assert((hotData.size() % 4 == 0) && "hotData size must be multiple of 4");

    [[maybe_unused]] auto buffer = _hotWriter.update(id.value(), hotData);
    return TrackView{id, hotData, std::nullopt, nullptr};
  }

  TrackView TrackStore::Writer::updateCold(TrackId id, std::span<std::byte const> coldData)
  {
    // Ensure size is multiple of 4 for LMDB
    assert((coldData.size() % 4 == 0) && "coldData size must be multiple of 4");

    [[maybe_unused]] auto buffer = _coldWriter.update(id.value(), coldData);
    return TrackView{id, std::span<std::byte const>{}, coldData, nullptr};
  }

  bool TrackStore::Writer::delHotCold(TrackId id)
  {
    bool hotDeleted = _hotWriter.del(id.value());
    bool coldDeleted = _coldWriter.del(id.value());
    return hotDeleted && coldDeleted;
  }

  std::optional<TrackView> TrackStore::Writer::getHot(TrackId id) const
  {
    auto optBuffer = _hotWriter.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackView{id, *optBuffer, std::nullopt, nullptr};
  }

  std::optional<TrackView> TrackStore::Writer::getCold(TrackId id) const
  {
    auto optColdBuffer = _coldWriter.get(id.value());
    if (!optColdBuffer || optColdBuffer->size() == 0) { return std::nullopt; }

    auto optHotBuffer = _hotWriter.get(id.value());
    if (!optHotBuffer || optHotBuffer->size() == 0) { return std::nullopt; }

    return TrackView{id, *optHotBuffer, *optColdBuffer, nullptr};
  }

} // namespace rs::core