// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <lmdb.h>

#include <cstddef>
#include <memory>
#include <string_view>

namespace ao::lmdb
{
  class Environment final
  {
  public:
    struct Options
    {
      unsigned int flags = 0;
      mdb_mode_t mode = 0644;
      MDB_dbi maxDatabases = 0;
      unsigned int maxReaders = 0;
      std::size_t mapSize = 0;
    };

    explicit Environment(std::string const& path);
    explicit Environment(std::string const& path, Options const& options);

    Environment(Environment const&) = delete;
    Environment& operator=(Environment const&) = delete;

    Environment(Environment&& other) noexcept;
    Environment& operator=(Environment&& other) noexcept;

    ~Environment() noexcept;

  private:
    std::unique_ptr<MDB_env, decltype([](auto* env) { mdb_env_close(env); })> _handle;

    friend class Database;
    friend class ReadTransaction;
    friend class WriteTransaction;
  };
}
