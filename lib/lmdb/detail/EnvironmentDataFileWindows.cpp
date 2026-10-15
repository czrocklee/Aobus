// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EnvironmentDataFile.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>

namespace ao::lmdb::detail
{
  namespace
  {
    constexpr auto kDataFileName = L"data.mdb";

    std::string systemMessage(DWORD const errorCode)
    {
      return std::error_code{static_cast<std::int32_t>(errorCode), std::system_category()}.message();
    }

    /// Whether @p errorCode means the filesystem has no sparse support at all.
    bool isSparseUnsupported(DWORD const errorCode) noexcept
    {
      // A volume whose driver does not implement the control answers with one of
      // these; anything else is a real failure of a control it does implement.
      return errorCode == ERROR_INVALID_FUNCTION || errorCode == ERROR_NOT_SUPPORTED;
    }
  } // namespace

  Result<> prepareEnvironmentDataFile(std::filesystem::path const& directory)
  {
    auto const dataPath = directory / kDataFileName;

    // OPEN_ALWAYS so a first open creates the file and later opens reuse it, and
    // the same share mode LMDB requests so both handles can be open at once.
    auto* const handle = ::CreateFileW(dataPath.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
      // LMDB could not open the same file for writing either, so failing here
      // only replaces its diagnostic with one that names the real step.
      auto const errorCode = ::GetLastError();
      return makeError(Error::Code::IoError,
                       std::format("Failed to open {}: {}", utility::pathToUtf8(dataPath), systemMessage(errorCode)));
    }

    // Marking an already sparse file is accepted, so repeat opens need no check
    // of their own.
    DWORD returnedBytes = 0;
    auto const marked = ::DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returnedBytes, nullptr);
    auto const markError = marked == FALSE ? ::GetLastError() : DWORD{0};
    std::ignore = ::CloseHandle(handle);

    if (marked != FALSE || isSparseUnsupported(markError))
    {
      // exFAT and the FAT family hold no holes. LMDB still opens there, and an
      // aborted transaction's extension simply stays allocated until reused.
      return {};
    }

    return makeError(
      Error::Code::IoError,
      std::format("Failed to make {} sparse: {}", utility::pathToUtf8(dataPath), systemMessage(markError)));
  }
} // namespace ao::lmdb::detail
