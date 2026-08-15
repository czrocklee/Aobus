// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/FileAllocation.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <utility>

namespace ao::utility
{
  std::uint64_t allocatedFileBytes(std::filesystem::path const& path)
  {
    // Sharing everything so inspecting a file the process already has open, such
    // as a live LMDB data file, does not fail on the sharing mode.
    auto* const handle = ::CreateFileW(path.c_str(),
                                       GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_EXISTING,
                                       0,
                                       nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
      return 0;
    }

    auto info = FILE_STANDARD_INFO{};
    auto const queried = ::GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof info);
    std::ignore = ::CloseHandle(handle);
    return queried == FALSE ? 0 : static_cast<std::uint64_t>(info.AllocationSize.QuadPart);
  }
} // namespace ao::utility
