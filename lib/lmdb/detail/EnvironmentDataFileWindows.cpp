// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "EnvironmentDataFile.h"
#include <ao/Error.h>
#include <ao/lmdb/Environment.h>

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
    bool sparseUnsupported(DWORD const errorCode) noexcept
    {
      // A volume whose driver does not implement the control answers with one of
      // these; anything else is a real failure of a control it does implement.
      return errorCode == ERROR_INVALID_FUNCTION || errorCode == ERROR_NOT_SUPPORTED;
    }

    /**
     * Reports what an existing data file already costs, touching nothing.
     *
     * Attributes come without a handle, so this needs no permission on the file
     * and cannot create one. An unreadable path is reported as the expensive
     * case: LMDB is about to fail the open with the real diagnostic, and until
     * something is known a large map is better assumed to cost its full size.
     */
    MapAllocation observedAllocation(std::filesystem::path const& dataPath) noexcept
    {
      auto const attributes = ::GetFileAttributesW(dataPath.c_str());

      if (attributes == INVALID_FILE_ATTRIBUTES)
      {
        return MapAllocation::WholeMap;
      }

      return (attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0 ? MapAllocation::OnDemand : MapAllocation::WholeMap;
    }
  } // namespace

  Result<MapAllocation> prepareEnvironmentDataFile(std::filesystem::path const& directory, DataFileAccess const access)
  {
    auto const dataPath = directory / kDataFileName;

    if (access == DataFileAccess::ReadOnly)
    {
      // Every step below is a mutation: it creates the file when it is absent,
      // demands write permission, and changes an attribute. None of that belongs
      // in opening a database for reading.
      return observedAllocation(dataPath);
    }

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
      return makeError(
        Error::Code::IoError, std::format("Failed to open {}: {}", dataPath.string(), systemMessage(errorCode)));
    }

    // Marking an already sparse file is accepted, so repeat opens need no check
    // of their own.
    DWORD returnedBytes = 0;
    auto const marked = ::DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returnedBytes, nullptr);
    auto const markError = marked == FALSE ? ::GetLastError() : DWORD{0};
    std::ignore = ::CloseHandle(handle);

    if (marked != FALSE)
    {
      return MapAllocation::OnDemand;
    }

    if (sparseUnsupported(markError))
    {
      // exFAT and the FAT family hold no holes. The map size becomes allocation
      // here, so the caller that owns capacity has to keep it modest rather than
      // rely on a hole this volume cannot give it.
      return MapAllocation::WholeMap;
    }

    return makeError(
      Error::Code::IoError, std::format("Failed to make {} sparse: {}", dataPath.string(), systemMessage(markError)));
  }
} // namespace ao::lmdb::detail
