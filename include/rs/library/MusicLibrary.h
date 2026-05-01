// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/library/DictionaryStore.h>
#include <rs/library/Meta.h>
#include <rs/library/MetaStore.h>
#include <rs/library/ListStore.h>
#include <rs/library/ResourceStore.h>
#include <rs/library/TrackStore.h>
#include <rs/lmdb/Database.h>

#include <filesystem>
#include <memory>

namespace rs::library
{
  class MusicLibrary final
  {
  public:
    explicit MusicLibrary(std::filesystem::path rootPath);

    rs::lmdb::ReadTransaction readTransaction() const { return rs::lmdb::ReadTransaction{_env}; }

    rs::lmdb::WriteTransaction writeTransaction() { return rs::lmdb::WriteTransaction{_env}; }

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
    rs::lmdb::Environment _env;
    rs::lmdb::WriteTransaction _txn;
    MetaStore _metaStore;
    MetaHeader _metaHeader{};
    TrackStore _tracks;
    ListStore _lists;
    ResourceStore _resources;
    DictionaryStore _dictionary;
  };
}
