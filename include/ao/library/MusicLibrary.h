// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/Type.h>
#include <ao/library/Meta.h>
#include <ao/lmdb/Transaction.h>

#include <filesystem>
#include <memory>

namespace ao::library
{
  class TrackStore;
  class ListStore;
  class ResourceStore;
  class DictionaryStore;

  /**
   * @brief High-level facade for the music library database.
   * Coordinates multiple specialized stores and manages the LMDB environment.
   */
  class MusicLibrary final
  {
  public:
    explicit MusicLibrary(std::filesystem::path rootPath);
    ~MusicLibrary();

    ao::lmdb::ReadTransaction readTransaction() const;
    ao::lmdb::WriteTransaction writeTransaction();

    TrackStore& tracks();
    TrackStore const& tracks() const;

    ListStore& lists();
    ListStore const& lists() const;

    ResourceStore& resources();
    ResourceStore const& resources() const;

    DictionaryStore& dictionary();
    DictionaryStore const& dictionary() const;

    MetaHeader const& metaHeader() const;

    std::filesystem::path const& rootPath() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
  };
}
