// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/lmdb/Transaction.h>

#include <filesystem>
#include <memory>

namespace ao::library
{
  struct MetaHeader;
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
    explicit MusicLibrary(std::filesystem::path musicRoot, std::filesystem::path databasePath);
    ~MusicLibrary();

    MusicLibrary(MusicLibrary const&) = delete;
    MusicLibrary& operator=(MusicLibrary const&) = delete;
    MusicLibrary(MusicLibrary&&) = delete;
    MusicLibrary& operator=(MusicLibrary&&) = delete;

    lmdb::ReadTransaction readTransaction() const;
    lmdb::WriteTransaction writeTransaction();

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

    MetaHeader const& metaHeader() const;
    void updateMetaHeader(MetaHeader const& header);

    std::filesystem::path const& rootPath() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
}
