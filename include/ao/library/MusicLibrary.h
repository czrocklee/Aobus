// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/WriteTransaction.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::library
{
  struct MetadataHeader;
  class LibraryWrite;
  class TrackStore;
  class ListStore;
  class ResourceStore;
  class DictionaryStore;
  class FileManifestStore;
  class WritableMusicLibrary;

  /**
   * @brief High-level facade for the music library database.
   * Coordinates multiple specialized stores and manages the LMDB environment.
   */
  class MusicLibrary final
  {
  public:
    struct Options final
    {
      static constexpr std::uint32_t kDefaultMaxReaders = 512;

      /**
       * Requested maximum number of simultaneous native read transactions.
       *
       * Zero skips LMDB's max-reader configuration. LMDB may grow its
       * persistent reader table only while this opener holds the exclusive
       * environment lock; a concurrent opener adopts the existing capacity,
       * and no opener shrinks it. The application default is higher because
       * independent runtime snapshots can overlap.
       */
      std::uint32_t maxReaders = kDefaultMaxReaders;

      /**
       * Pins the database's capacity at exactly this many bytes, with no growth.
       *
       * Zero instead selects managed capacity: an existing database keeps the
       * capacity it recorded, a fresh one starts at the library floor, and each
       * open raises the map when the recorded peak has come too close to it. A
       * nonzero value is for callers that need one known capacity, including
       * tests that have to reach it.
       */
      std::uint64_t pinnedMapBytes = 0;
    };

    /**
     * @brief How much this database may hold, and how far it has already grown.
     *
     * `highWaterBytes` is the peak the environment has needed, not a measure of
     * live data: deleted rows return their pages to the free list for reuse
     * without lowering it. It is the figure a capacity decision reads, because
     * the map has to cover the peak rather than the survivors.
     */
    struct StorageCapacity final
    {
      std::uint64_t mapBytes = 0;
      std::uint64_t highWaterBytes = 0;
    };

    ~MusicLibrary();

    static Result<MusicLibrary> open(std::filesystem::path musicRoot, std::filesystem::path databasePath);
    static Result<MusicLibrary> open(std::filesystem::path musicRoot,
                                     std::filesystem::path databasePath,
                                     Options options);

    MusicLibrary(MusicLibrary const&) = delete;
    MusicLibrary& operator=(MusicLibrary const&) = delete;
    MusicLibrary(MusicLibrary&&) noexcept;
    MusicLibrary& operator=(MusicLibrary&&) noexcept;

    ReadTransaction readTransaction() const;
    std::uint64_t libraryRevision(ReadTransaction const& transaction) const;
    std::uint64_t libraryRevision(LibraryWrite const& write) const;
    std::uint64_t libraryRevision(WriteTransaction const& transaction) const;
    MetadataHeader metadataHeader() const;
    MetadataHeader metadataHeader(ReadTransaction const& transaction) const;
    MetadataHeader metadataHeader(LibraryWrite const& write) const;
    MetadataHeader metadataHeader(WriteTransaction const& transaction) const;

    TrackStore const& tracks() const;

    ListStore const& lists() const;

    ResourceStore const& resources() const;

    DictionaryStore const& dictionary() const;

    FileManifestStore const& manifest() const;

    std::filesystem::path const& rootPath() const;
    std::filesystem::path const& databasePath() const;

    StorageCapacity storageCapacity() const;

  private:
    MusicLibrary() = default;

    Result<> initialize(std::filesystem::path musicRoot, std::filesystem::path databasePath, Options options);
    WriteTransaction beginWriteTransaction(WriteTransaction::Options options,
                                           std::shared_ptr<void const> writerSessionAnchorPtr);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class WritableMusicLibrary;
  };
} // namespace ao::library
