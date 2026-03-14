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
#include <rs/lmdb/Transaction.h>

#include <filesystem>
#include <memory>

namespace rs::core
{
  class MusicLibrary
  {
  public:
    using TrackId = TrackStore::Id;

    explicit MusicLibrary(std::filesystem::path rootPath);

    rs::lmdb::ReadTransaction readTransaction() const { return rs::lmdb::ReadTransaction{_env}; }

    rs::lmdb::WriteTransaction writeTransaction() { return rs::lmdb::WriteTransaction{_env}; }

    TrackStore& tracks() { return _tracks; }
    const TrackStore& tracks() const { return _tracks; }

    ListStore& lists() { return _lists; }
    const ListStore& lists() const { return _lists; }

    ResourceStore& resources() { return _resources; }
    const ResourceStore& resources() const { return _resources; }

    Dictionary& dictionary() { return *_dictionary; }
    const Dictionary& dictionary() const { return *_dictionary; }

    const std::filesystem::path& rootPath() const { return _root; }

  private:
    const std::filesystem::path _root;
    rs::lmdb::Environment _env;
    TrackStore _tracks;
    ListStore _lists;
    ResourceStore _resources;
    std::unique_ptr<Dictionary> _dictionary;
    lmdb::MDB _dictReadDb;
    lmdb::MDB _dictWriteDb;
  };
}
