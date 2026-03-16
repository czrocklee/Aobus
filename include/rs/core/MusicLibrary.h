// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Dictionary.h>
#include <rs/core/ListStore.h>
#include <rs/core/ResourceStore.h>
#include <rs/core/TrackStore.h>
#include <rs/lmdb/Database.h>

#include <filesystem>
#include <memory>

namespace rs::core
{
  class MusicLibrary
  {
  public:
    using TrackId = TrackStore::Id;

    explicit MusicLibrary(std::filesystem::path rootPath);

    [[nodiscard]] rs::lmdb::ReadTransaction readTransaction() const { return rs::lmdb::ReadTransaction{_env}; }

    [[nodiscard]] rs::lmdb::WriteTransaction writeTransaction() { return rs::lmdb::WriteTransaction{_env}; }

    [[nodiscard]] TrackStore& tracks() { return _tracks; }
    [[nodiscard]] TrackStore const& tracks() const { return _tracks; }

    [[nodiscard]] ListStore& lists() { return _lists; }
    [[nodiscard]] ListStore const& lists() const { return _lists; }

    [[nodiscard]] ResourceStore& resources() { return _resources; }
    [[nodiscard]] ResourceStore const& resources() const { return _resources; }

    [[nodiscard]] Dictionary& dictionary() { return _dictionary; }
    [[nodiscard]] Dictionary const& dictionary() const { return _dictionary; }

    [[nodiscard]] std::filesystem::path const& rootPath() const { return _root; }

  private:
    std::filesystem::path const _root;
    rs::lmdb::Environment _env;
    rs::lmdb::WriteTransaction _txn;
    TrackStore _tracks;
    ListStore _lists;
    ResourceStore _resources;
    Dictionary _dictionary;
  };
}
