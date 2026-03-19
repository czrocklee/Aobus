// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/core/TrackStore.h>

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
  {
  }

  TrackStore::Reader::Iterator TrackStore::Reader::begin() const
  {
    if (auto iter = _hotReader.begin(); iter != _hotReader.end())
    {
      [[maybe_unused]] auto&& [id, buffer] = *iter;
      return Iterator{std::move(iter)};
    }
    return end();
  }

  TrackStore::Reader::Iterator TrackStore::Reader::end() const
  {
    return Iterator{_hotReader.end()};
  }

  std::optional<TrackView> TrackStore::Reader::get(TrackId id) const
  {
    auto optBuffer = _hotReader.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackView{*optBuffer};
  }

  // HotProxy implementation
  std::optional<TrackHotView> TrackStore::Reader::HotProxy::get(TrackId id) const
  {
    auto optBuffer = _reader._hotReader.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackHotView{*optBuffer};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::HotProxy::begin() const
  {
    return _reader.begin();
  }

  TrackStore::Reader::Iterator TrackStore::Reader::HotProxy::end() const
  {
    return _reader.end();
  }

  // ColdProxy implementation
  std::optional<TrackColdView> TrackStore::Reader::ColdProxy::get(TrackId id) const
  {
    auto optBuffer = _reader._coldReader.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackColdView{*optBuffer};
  }

  TrackStore::Reader::Iterator TrackStore::Reader::ColdProxy::begin() const
  {
    return _reader.begin();
  }

  TrackStore::Reader::Iterator TrackStore::Reader::ColdProxy::end() const
  {
    return _reader.end();
  }

  // TrackStore::Reader::Iterator implementation
  TrackStore::Reader::Iterator::Iterator(lmdb::Database::Reader::Iterator&& iter) : _iter{std::move(iter)}
  {
  }

  bool TrackStore::Reader::Iterator::operator==(Iterator const& other) const
  {
    return _iter == other._iter;
  }

  TrackStore::Reader::Iterator& TrackStore::Reader::Iterator::operator++()
  {
    ++_iter;
    return *this;
  }

  TrackStore::Reader::Iterator::value_type TrackStore::Reader::Iterator::operator*() const
  {
    auto&& [id, buffer] = *_iter;
    return {TrackId{(id)}, TrackView(buffer)};
  }

  // TrackStore::Writer implementation
  TrackStore::Writer::Writer(lmdb::Database::Writer&& hotWriter, lmdb::Database::Writer&& coldWriter)
    : _hotWriter{std::move(hotWriter)}
    , _coldWriter{std::move(coldWriter)}
  {
  }

  std::pair<TrackId, TrackView> TrackStore::Writer::create(std::span<std::byte const> data)
  {
    auto [id, buffer] = _hotWriter.append(data);
    return {TrackId{id}, TrackView{data}};
  }

  TrackView TrackStore::Writer::update(TrackId id, std::span<std::byte const> data)
  {
    [[maybe_unused]] auto buffer = _hotWriter.update(id.value(), data);
    return TrackView{data};
  }

  bool TrackStore::Writer::del(TrackId id)
  {
    return _hotWriter.del(id.value());
  }

  std::optional<TrackView> TrackStore::Writer::get(TrackId id) const
  {
    auto optBuffer = _hotWriter.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackView{*optBuffer};
  }

  // Hot/Cold split methods
  std::pair<TrackId, TrackHotView> TrackStore::Writer::createHotCold(
      std::span<std::byte const> hotData,
      std::span<std::byte const> coldData)
  {
    auto [id, hotBuffer] = _hotWriter.append(hotData);
    [[maybe_unused]] auto [coldId, coldBuffer] = _coldWriter.append(coldData);
    return {TrackId{id}, TrackHotView{hotData}};
  }

  TrackHotView TrackStore::Writer::updateHot(TrackId id, std::span<std::byte const> hotData)
  {
    [[maybe_unused]] auto buffer = _hotWriter.update(id.value(), hotData);
    return TrackHotView{hotData};
  }

  TrackColdView TrackStore::Writer::updateCold(TrackId id, std::span<std::byte const> coldData)
  {
    [[maybe_unused]] auto buffer = _coldWriter.update(id.value(), coldData);
    return TrackColdView{coldData};
  }

  bool TrackStore::Writer::delHotCold(TrackId id)
  {
    bool hotDeleted = _hotWriter.del(id.value());
    bool coldDeleted = _coldWriter.del(id.value());
    return hotDeleted && coldDeleted;
  }

  std::optional<TrackHotView> TrackStore::Writer::getHot(TrackId id) const
  {
    auto optBuffer = _hotWriter.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackHotView{*optBuffer};
  }

  std::optional<TrackColdView> TrackStore::Writer::getCold(TrackId id) const
  {
    auto optBuffer = _coldWriter.get(id.value());
    if (!optBuffer || optBuffer->size() == 0) { return std::nullopt; }
    return TrackColdView{*optBuffer};
  }

} // namespace rs::core