// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FileAllocation.h>

#include <cstdint>
#include <filesystem>
#include <sys/stat.h>

namespace ao::utility
{
  namespace
  {
    // st_blocks counts units of this size by definition, whatever the
    // filesystem's own block size happens to be.
    constexpr std::uint64_t kStatBlockBytes = 512;
  } // namespace

  std::uint64_t allocatedFileBytes(std::filesystem::path const& path)
  {
    // POSIX declares a stat() function that hides the struct name, so the type
    // needs an alias before it can be spelled without the elaborated specifier.
    using FileStatus = struct stat;
    auto status = FileStatus{};

    if (::stat(path.c_str(), &status) != 0)
    {
      return 0;
    }

    return static_cast<std::uint64_t>(status.st_blocks) * kStatBlockBytes;
  }
} // namespace ao::utility
