// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::lmdb::detail
{
  /**
   * UnvalidatedDatabase - Source-private DBI used only to read a stable
   * version prefix before the current schema's exact key flags are known.
   */
  class UnvalidatedDatabase final
  {
  public:
    static Result<UnvalidatedDatabase> openExisting(WriteTransaction& transaction, std::string const& name);

    ~UnvalidatedDatabase() = default;
    UnvalidatedDatabase(UnvalidatedDatabase const&) = delete;
    UnvalidatedDatabase& operator=(UnvalidatedDatabase const&) = delete;
    UnvalidatedDatabase(UnvalidatedDatabase&& other) noexcept;
    UnvalidatedDatabase& operator=(UnvalidatedDatabase&& other) noexcept;

    std::optional<std::span<std::byte const>> getRaw(ReadTransaction const& transaction,
                                                     std::span<std::byte const> key) const;

    Result<IntegerKeyDatabase> intoIntegerKey(std::string_view databaseName) &&;

  private:
    UnvalidatedDatabase(DbiHandle dbi, std::uint32_t nativeFlags) noexcept
      : _dbi{dbi}, _nativeFlags{nativeFlags}
    {
    }

    DbiHandle _dbi = std::numeric_limits<DbiHandle>::max();
    std::uint32_t _nativeFlags = 0;
  };
} // namespace ao::lmdb::detail
