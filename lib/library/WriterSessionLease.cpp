// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WriterSessionLease.h"

#include "LockFile.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <expected>
#include <filesystem>
#include <format>
#include <utility>

namespace ao::library::detail
{
  Result<WriterSessionLease> WriterSessionLease::acquire(std::filesystem::path const& databasePath)
  {
    auto lockRes = LockFile::acquire(databasePath / utility::pathFromUtf8(kFileName), LockFile::Mode::Exclusive);

    if (!lockRes)
    {
      if (lockRes.error().code == Error::Code::ResourceBusy)
      {
        return makeError(Error::Code::Conflict,
                         std::format("Library already has a writable process: {}", utility::pathToUtf8(databasePath)));
      }

      return std::unexpected{lockRes.error()};
    }

    return WriterSessionLease{std::move(*lockRes)};
  }

  WriterSessionLease::WriterSessionLease(LockFile lock) noexcept
    : _lock{std::move(lock)}
  {
  }
} // namespace ao::library::detail
