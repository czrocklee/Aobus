// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/library/FileTimeConversion.h>

#include "FileClockConversion.h"
#include <ao/Error.h>
#include <ao/FileTimestamp.h>

#include <filesystem>
#include <system_error>

namespace ao::library
{
  Result<FileTimestamp> fileTimestampFromFileTime(std::filesystem::file_time_type const time)
  {
    return detail::fileTimestampFromClockTime(time);
  }

  Result<FileTimestamp> lastWriteTimestamp(std::filesystem::path const& path)
  {
    auto ec = std::error_code{};
    auto const lastWriteTime = std::filesystem::last_write_time(path, ec);

    if (ec)
    {
      return makeError(Error::Code::IoError, ec.message());
    }

    return fileTimestampFromFileTime(lastWriteTime);
  }
} // namespace ao::library
