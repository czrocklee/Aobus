// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/library/DictionaryStore.h>
#include <ao/library/ListStore.h>
#include <ao/library/Meta.h>
#include <ao/library/MetaStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Database.h>

#include <filesystem>
#include <memory>

namespace ao::library
{
  class MusicLibrary final
  {
  public:
    explicit MusicLibrary(std::filesystem::path rootPath);

    ao::lmdb::ReadTransaction readTransaction() const { return ao::lmdb::ReadTransaction{_env}; }

    ao::lmdb::WriteTransaction writeTransaction() { return ao::lmdb::WriteTransaction{_env}; }

    TrackStore& tracks() { return _tracks; }
    TrackStore const& tracks() const { return _tracks; }

    ListStore& lists() { return _lists; }
    ListStore const& lists() const { return _lists; }

    ResourceStore& resources() { return _resources; }
    ResourceStore const& resources() const { return _resources; }

    DictionaryStore& dictionary() { return _dictionary; }
    DictionaryStore const& dictionary() const { return _dictionary; }

    MetaHeader const& metaHeader() const { return _metaHeader; }

    std::filesystem::path const& rootPath() const { return _root; }

  private:
    std::filesystem::path const _root;
    ao::lmdb::Environment _env;
    ao::lmdb::WriteTransaction _txn;
    MetaStore _metaStore;
    MetaHeader _metaHeader{};
    TrackStore _tracks;
    ListStore _lists;
    ResourceStore _resources;
    DictionaryStore _dictionary;
  };
}
