// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/ThrowError.h"
#include <ao/lmdb/Environment.h>

#include <lmdb.h>

#include <string>
#include <type_traits>

namespace ao::lmdb
{
  // The public header mirrors these native LMDB types without including <lmdb.h>;
  // assert the mirrors stay in sync with the real ABI.
  static_assert(std::is_same_v<DbiHandle, ::MDB_dbi>);
  static_assert(std::is_same_v<EnvMode, ::mdb_mode_t>);
  static_assert(kEnvNoTls == MDB_NOTLS);

  void Environment::MdbEnvDeleter::operator()(MDB_env* env) const noexcept
  {
    ::mdb_env_close(env);
  }

  Environment::Environment(std::string const& path)
    : Environment{path, Options{}}
  {
  }

  Environment::Environment(std::string const& path, Environment::Options const& options)
  {
    ::MDB_env* handle = nullptr;
    throwOnError("mdb_env_create", ::mdb_env_create(&handle));
    _envPtr.reset(handle);

    if (options.mapSize > 0)
    {
      throwOnError("mdb_env_set_mapsize", ::mdb_env_set_mapsize(_envPtr.get(), options.mapSize));
    }

    if (options.maxDatabases > 0)
    {
      throwOnError("mdb_env_set_maxdbs", ::mdb_env_set_maxdbs(_envPtr.get(), options.maxDatabases));
    }

    if (options.maxReaders > 0)
    {
      throwOnError("mdb_env_set_maxreaders", ::mdb_env_set_maxreaders(_envPtr.get(), options.maxReaders));
    }

    throwOnError("mdb_env_open", ::mdb_env_open(_envPtr.get(), path.c_str(), options.flags, options.mode));
  }

  Environment::Environment(Environment&& other) noexcept = default;

  Environment& Environment::operator=(Environment&& other) noexcept = default;

  Environment::~Environment() noexcept = default;
}
