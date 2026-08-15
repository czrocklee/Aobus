// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/utility/PlatformDirectories.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ao::utility
{
  namespace
  {
    /// Windows application-data directories are conventionally capitalized.
    constexpr auto kApplicationDirectoryName = std::wstring_view{L"Aobus"};

    /**
     * @brief Reads @p name from the wide environment.
     *
     * The narrow `getenv` family converts through the active code page, which
     * silently mangles any profile path holding characters that page cannot
     * represent. Configuration lives under the user profile, so the wide API is
     * the only correct reader here.
     *
     * An unset variable and an empty one are both reported as absent, matching
     * the POSIX implementation.
     */
    std::optional<std::filesystem::path> environmentPath(wchar_t const* name)
    {
      auto buffer = std::wstring(MAX_PATH, L'\0');
      auto length = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));

      if (length >= buffer.size())
      {
        // The first call reports the size it needs, including the terminator.
        buffer.resize(length);
        length = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
      }

      if (length == 0 || length >= buffer.size())
      {
        return std::nullopt;
      }

      auto path = std::filesystem::path{std::wstring_view{buffer.data(), length}};

      if (!path.is_absolute())
      {
        // Covers both a plain relative value and a drive-relative one such as
        // `C:config`, which resolves against that drive's current directory.
        // Either would scatter configuration outside the profile.
        return std::nullopt;
      }

      return path;
    }
  } // namespace

  Result<std::filesystem::path> applicationConfigDirectory()
  {
    if (auto optPath = environmentPath(L"LOCALAPPDATA"); optPath)
    {
      return *optPath / kApplicationDirectoryName;
    }

    if (auto optPath = environmentPath(L"APPDATA"); optPath)
    {
      return *optPath / kApplicationDirectoryName;
    }

    return makeError(Error::Code::NotFound, "Neither LOCALAPPDATA nor APPDATA names a configuration directory");
  }
} // namespace ao::utility
