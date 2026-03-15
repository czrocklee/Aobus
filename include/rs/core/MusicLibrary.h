/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

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

    [[nodiscard]] Dictionary& dictionary() { return *_dictionary; }
    [[nodiscard]] Dictionary const& dictionary() const { return *_dictionary; }

    [[nodiscard]] std::filesystem::path const& rootPath() const { return _root; }

  private:
    std::filesystem::path const _root;
    rs::lmdb::Environment _env;
    TrackStore _tracks;
    ListStore _lists;
    ResourceStore _resources;
    std::unique_ptr<Dictionary> _dictionary;
    lmdb::Database _dictReadDb;
    lmdb::Database _dictWriteDb;
  };
}
