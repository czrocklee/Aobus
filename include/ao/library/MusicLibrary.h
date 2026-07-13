// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Transaction.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace ao::library
{
  struct MetadataHeader;
  class MetadataStore;
  class TrackStore;
  class ListStore;
  class ResourceStore;
  class DictionaryStore;
  class FileManifestStore;

  /**
   * @brief High-level facade for the music library database.
   * Coordinates multiple specialized stores and manages the LMDB environment.
   */
  class MusicLibrary final
  {
  public:
    struct Options final
    {
      // Zero selects the production default map size.
      std::size_t mapSize = 0;
    };

    explicit MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath);
    MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath, Options options);
    ~MusicLibrary();

    static Result<MusicLibrary> open(std::filesystem::path musicRoot, std::filesystem::path databasePath);
    static Result<MusicLibrary> open(std::filesystem::path musicRoot,
                                     std::filesystem::path databasePath,
                                     Options options);

    MusicLibrary(MusicLibrary const&) = delete;
    MusicLibrary& operator=(MusicLibrary const&) = delete;
    MusicLibrary(MusicLibrary&&) noexcept;
    MusicLibrary& operator=(MusicLibrary&&) noexcept;

    lmdb::ReadTransaction readTransaction() const;
    lmdb::WriteTransaction writeTransaction();
    std::uint64_t libraryRevision(lmdb::ReadTransaction const& transaction) const;
    std::uint64_t libraryRevision(lmdb::WriteTransaction& transaction) const;

    TrackStore& tracks();
    TrackStore const& tracks() const;

    ListStore& lists();
    ListStore const& lists() const;

    ResourceStore& resources();
    ResourceStore const& resources() const;

    DictionaryStore& dictionary();
    DictionaryStore const& dictionary() const;

    FileManifestStore& manifest();
    FileManifestStore const& manifest() const;

    MetadataStore& metadata();
    MetadataStore const& metadata() const;
    MetadataHeader metadataHeader() const;

    std::filesystem::path const& rootPath() const;

  private:
    MusicLibrary() = default;

    Result<> initialize(std::filesystem::path musicRoot, std::filesystem::path databasePath, Options options);

    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::library
