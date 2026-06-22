// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/ResultError.h"
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

#include <lmdb.h>

#include <string>
#include <type_traits>
#include <utility>

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

  Result<Environment> Environment::open(std::string const& path)
  {
    return open(path, Options{});
  }

  Result<Environment> Environment::open(std::string const& path, Environment::Options const& options)
  {
    ::MDB_env* handle = nullptr;

    if (auto result = resultFromCode("mdb_env_create", ::mdb_env_create(&handle)); !result)
    {
      return makeError(result.error().code, result.error().message);
    }

    auto envPtr = EnvPtr{handle};

    if (options.mapSize > 0)
    {
      if (auto result = resultFromCode("mdb_env_set_mapsize", ::mdb_env_set_mapsize(envPtr.get(), options.mapSize));
          !result)
      {
        return makeError(result.error().code, result.error().message);
      }
    }

    if (options.maxDatabases > 0)
    {
      if (auto result = resultFromCode("mdb_env_set_maxdbs", ::mdb_env_set_maxdbs(envPtr.get(), options.maxDatabases));
          !result)
      {
        return makeError(result.error().code, result.error().message);
      }
    }

    if (options.maxReaders > 0)
    {
      if (auto result =
            resultFromCode("mdb_env_set_maxreaders", ::mdb_env_set_maxreaders(envPtr.get(), options.maxReaders));
          !result)
      {
        return makeError(result.error().code, result.error().message);
      }
    }

    if (auto result =
          resultFromCode("mdb_env_open", ::mdb_env_open(envPtr.get(), path.c_str(), options.flags, options.mode));
        !result)
    {
      return makeError(result.error().code, result.error().message);
    }

    return Environment{std::move(envPtr)};
  }

  Environment::Environment(EnvPtr envPtr)
    : _envPtr{std::move(envPtr)}
  {
  }

  Environment::Environment(Environment&& other) noexcept = default;

  Environment& Environment::operator=(Environment&& other) noexcept = default;

  Environment::~Environment() noexcept = default;
}
