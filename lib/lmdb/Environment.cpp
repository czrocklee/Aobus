// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ThrowError.h"
#include <ao/lmdb/Environment.h>

namespace ao::lmdb
{
  Environment::Environment(std::string const& path)
    : Environment{path, Options{}}
  {
  }

  Environment::Environment(std::string const& path, Environment::Options const& options)
  {
    ::MDB_env* handle = nullptr;
    throwOnError("mdb_env_create", ::mdb_env_create(&handle));
    _handle.reset(handle);

    if (options.mapSize > 0)
    {
      throwOnError("mdb_env_set_mapsize", ::mdb_env_set_mapsize(_handle.get(), options.mapSize));
    }

    if (options.maxDatabases > 0)
    {
      throwOnError("mdb_env_set_maxdbs", ::mdb_env_set_maxdbs(_handle.get(), options.maxDatabases));
    }

    if (options.maxReaders > 0)
    {
      throwOnError("mdb_env_set_maxreaders", ::mdb_env_set_maxreaders(_handle.get(), options.maxReaders));
    }

    throwOnError("mdb_env_open", ::mdb_env_open(_handle.get(), path.c_str(), options.flags, options.mode));
  }

  Environment::Environment(Environment&& other) noexcept = default;

  Environment& Environment::operator=(Environment&& other) noexcept = default;

  Environment::~Environment() noexcept = default;
}
