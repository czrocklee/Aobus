// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "LockFile.h"
#include <ao/Error.h>

#include <filesystem>
#include <string_view>

namespace ao::library::detail
{
  /// The exclusive lock that admits one writer process to a database at a time.
  class WriterSessionLease final
  {
  public:
    static constexpr std::string_view kFileName = ".aobus-writer.lock";

    /// Fails as `Conflict` while another writer session holds the lease.
    static Result<WriterSessionLease> acquire(std::filesystem::path const& databasePath);

  private:
    explicit WriterSessionLease(LockFile lock) noexcept;

    LockFile _lock;
  };
} // namespace ao::library::detail
