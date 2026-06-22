// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// LMDB native handle, kept opaque so <lmdb.h> stays out of public headers (it
// otherwise bleeds into 40+ translation units). The real definition is pulled in
// by the lmdb wrapper .cpp files.
struct MDB_env;

namespace ao::lmdb
{
  // Mirror of LMDB's native integer typedefs so callers need not include
  // <lmdb.h>. Equivalence with MDB_dbi / mdb_mode_t is asserted in the .cpp.
  using DbiHandle = unsigned int; // == MDB_dbi
  using EnvMode = unsigned int;   // == mdb_mode_t

  // Mirror of the MDB_NOTLS env flag (the only flag consumers configure). The
  // value is verified against the real macro in Environment.cpp.
  inline constexpr std::uint32_t kEnvNoTls = 0x200000U;

  constexpr EnvMode kDefaultEnvironmentMode = 0644;

  class Environment final
  {
  public:
    struct Options
    {
      std::uint32_t flags = 0;
      EnvMode mode = kDefaultEnvironmentMode;
      DbiHandle maxDatabases = 0;
      std::uint32_t maxReaders = 0;
      std::size_t mapSize = 0;
    };

    static Result<Environment> open(std::string const& path);
    static Result<Environment> open(std::string const& path, Options const& options);

    Environment(Environment const&) = delete;
    Environment& operator=(Environment const&) = delete;

    Environment(Environment&& other) noexcept;
    Environment& operator=(Environment&& other) noexcept;

    ~Environment() noexcept;

    MDB_env* handle() const noexcept { return _envPtr.get(); }

  private:
    struct MdbEnvDeleter final
    {
      void operator()(MDB_env* env) const noexcept;
    };

    using EnvPtr = std::unique_ptr<MDB_env, MdbEnvDeleter>;

    explicit Environment(EnvPtr envPtr);

    EnvPtr _envPtr;

    friend class Database;
    friend class ReadTransaction;
    friend class WriteTransaction;
  };
} // namespace ao::lmdb
