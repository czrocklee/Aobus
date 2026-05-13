// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Database.h>

#include <functional>
#include <gsl-lite/gsl-lite.hpp>
#include <optional>
#include <span>
#include <vector>

namespace ao::library
{
  /**
   * TrackStore - Binary storage for tracks using hot/cold separation.
   *
   * Uses two LMDB databases:
   * - tracks_hot: TrackHotHeader + payload (hot fields for fast filtering)
   * - tracks_cold: TrackColdHeader + custom KV + uri (cold fields)
   * - Key: uint32_t track ID (same ID links hot and cold)
   */
  class TrackStore final
  {
  public:
    class Reader;
    class Writer;

    explicit TrackStore(ao::lmdb::Database hotDb, ao::lmdb::Database coldDb);

    Reader reader(ao::lmdb::ReadTransaction const& txn) const;
    Writer writer(ao::lmdb::WriteTransaction& txn);

  private:
    ao::lmdb::Database _hotDb;
    ao::lmdb::Database _coldDb;
  };

  /**
   * TrackStore::Reader - Read-only access to tracks.
   */
  class TrackStore::Reader final
  {
  public:
    class Iterator;

    /**
     * LoadMode - Controls which data is loaded for each track.
     */
    enum class LoadMode
    {
      Hot,  // Only hot data
      Cold, // Only cold data
      Both  // Both hot and cold
    };

    Iterator begin(LoadMode mode = LoadMode::Both) const;
    Iterator end(LoadMode mode = LoadMode::Both) const;

    /**
     * Get a track by ID.
     * @return TrackView or std::nullopt if not found
     */
    std::optional<TrackView> get(TrackId id, LoadMode mode = LoadMode::Both) const;

  private:
    Reader(ao::lmdb::Database::Reader hotReader, ao::lmdb::Database::Reader coldReader);

    ao::lmdb::Database::Reader _hotReader;
    ao::lmdb::Database::Reader _coldReader;
    friend class TrackStore;
  };

  /**
   * TrackStore::Reader::Iterator - Iterator over tracks.
   */
  class TrackStore::Reader::Iterator final
  {
  public:
    using value_type = std::pair<TrackId, TrackView>;

    Iterator() = default;
    Iterator(Iterator const&) = default;
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
    ~Iterator() = default;
    Iterator(Iterator&&) = default;

    Iterator& operator=(Iterator const&) = default;
    Iterator& operator=(Iterator&&) = default;

    bool operator==(Iterator const& other) const;
    Iterator& operator++();
    value_type operator*() const;

  private:
    Iterator(ao::lmdb::Database::Reader::Iterator&& hotIter,
             ao::lmdb::Database::Reader::Iterator&& coldIter,
             Reader::LoadMode mode);

    std::optional<ao::lmdb::Database::Reader::Iterator> _optHotIter;
    std::optional<ao::lmdb::Database::Reader::Iterator> _optColdIter;
    Reader::LoadMode _mode = Reader::LoadMode::Both;
    friend class Reader;
  };

  /**
   * TrackStore::Writer - Write access to tracks.
   */
  class TrackStore::Writer final
  {
  public:
    /**
     * Get track by ID with specified load mode.
     */
    std::optional<TrackView> get(TrackId id, Reader::LoadMode mode) const;

    /**
     * Create a new track with hot and cold data.
     * @param hotData TrackHotHeader + payload
     * @param coldData TrackColdHeader + custom KV + uri
     * @return Pair of (track ID, TrackView)
     */
    std::pair<TrackId, TrackView> createHotCold(std::span<std::byte const> hotData,
                                                std::span<std::byte const> coldData);

    /**
     * Zero-copy create: reserves space and calls fill callback to populate spans.
     * @param hotSize Size of hot data (must be multiple of 4)
     * @param coldSize Size of cold data (must be multiple of 4)
     * @param fill Callback: fill(TrackId id, span<byte> hot, span<byte> cold) -> void
     * @return Pair of (track ID, TrackView)
     */
    template<class F>
    std::pair<TrackId, TrackView> createHotCold(std::size_t hotSize, std::size_t coldSize, F&& fill);

    /**
     * Update hot track data.
     */
    void updateHot(TrackId id, std::span<std::byte const> hotData);

    /**
     * Zero-copy updateHot: reserves space and calls fill callback to populate span.
     * @param id Track ID to update
     * @param size Size of hot data (must be multiple of 4)
     * @param fill Callback: fill(span<byte> hot) -> void
     */
    template<class F>
    void updateHot(TrackId id, std::size_t size, F&& fill);

    /**
     * Update cold track data.
     */
    void updateCold(TrackId id, std::span<std::byte const> coldData);

    /**
     * Zero-copy updateCold: reserves space and calls fill callback to populate span.
     * @param id Track ID to update
     * @param size Size of cold data (must be multiple of 4)
     * @param fill Callback: fill(span<byte> cold) -> void
     */
    template<class F>
    void updateCold(TrackId id, std::size_t size, F&& fill);

    /**
     * Delete both hot and cold track data.
     */
    bool remove(TrackId id);

    /**
     * Clear all tracks.
     */
    void clear();

  private:
    explicit Writer(ao::lmdb::Database::Writer&& hotWriter, ao::lmdb::Database::Writer&& coldWriter);

    ao::lmdb::Database::Writer _hotWriter;
    ao::lmdb::Database::Writer _coldWriter;
    friend class TrackStore;
  };

  // Template implementations

  template<class F>
  std::pair<TrackId, TrackView> TrackStore::Writer::createHotCold(std::size_t hotSize, std::size_t coldSize, F&& fill)
  {
    gsl_Expects((hotSize % 4) == 0);
    gsl_Expects((coldSize % 4) == 0);

    // Reserve hot span and get auto-increment ID
    auto [id, hotSpan] = _hotWriter.append(hotSize);

    // Reserve cold at the SAME explicit ID (not append)
    auto coldSpan = _coldWriter.create(id, coldSize);

    // Populate both spans via callback
    fill(TrackId{id}, hotSpan, coldSpan);

    return {TrackId{id}, TrackView{hotSpan, coldSpan}};
  }

  template<class F>
  void TrackStore::Writer::updateHot(TrackId id, std::size_t size, F&& fill)
  {
    gsl_Expects((size % 4) == 0);

    auto span = _hotWriter.update(id.value(), size);
    fill(span);
  }

  template<class F>
  void TrackStore::Writer::updateCold(TrackId id, std::size_t size, F&& fill)
  {
    gsl_Expects((size % 4) == 0);

    auto span = _coldWriter.update(id.value(), size);
    fill(span);
  }
} // namespace ao::library
