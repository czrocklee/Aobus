// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/utility/Path.h>
#include <ao/utility/PlatformDirectories.h>

#include <pwd.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::utility
{
  namespace
  {
    constexpr std::size_t kPasswdBufferLength = 1024;
    constexpr auto kApplicationDirectoryName = std::string_view{"aobus"};

    std::optional<std::filesystem::path> environmentPath(char const* name)
    {
      // NOLINTNEXTLINE(concurrency-mt-unsafe): read during composition, before worker threads exist.
      auto const* const value = std::getenv(name);

      if (value == nullptr || *value == '\0')
      {
        return std::nullopt;
      }

      auto path = pathFromNative(value);

      if (!path.is_absolute())
      {
        // A relative value resolves against the working directory, so honoring
        // it would scatter configuration wherever the process happened to be
        // launched from. Treat it as unset and let the next candidate answer.
        return std::nullopt;
      }

      return path;
    }

    /**
     * @brief The account's home directory, for a session that exports no `HOME`.
     *
     * GLib consults the password database in this case, and GTK reaches this
     * helper through the same public entry point, so dropping the fallback here
     * would make a service-style or `env -i` session lose its configuration.
     */
    std::optional<std::filesystem::path> passwdHomeDirectory()
    {
      auto buffer = std::array<char, kPasswdBufferLength>{};
      auto entry = ::passwd{};
      ::passwd* result = nullptr;

      if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result) != 0 || result == nullptr)
      {
        return std::nullopt;
      }

      if (result->pw_dir == nullptr || *result->pw_dir == '\0')
      {
        return std::nullopt;
      }

      auto home = pathFromNative(result->pw_dir);

      return home.is_absolute() ? std::optional{std::move(home)} : std::nullopt;
    }

    Result<std::filesystem::path> applicationDirectory(char const* variable, std::string_view fallback)
    {
      if (auto optRoot = environmentPath(variable); optRoot)
      {
        return *optRoot / pathFromUtf8(kApplicationDirectoryName);
      }

      if (auto optHome = environmentPath("HOME").or_else(passwdHomeDirectory); optHome)
      {
        return *optHome / pathFromUtf8(fallback) / pathFromUtf8(kApplicationDirectoryName);
      }

      return makeError(Error::Code::NotFound, "No environment or account entry names a home directory");
    }
  } // namespace

  Result<std::filesystem::path> applicationConfigDirectory()
  {
    return applicationDirectory("XDG_CONFIG_HOME", ".config");
  }

  Result<std::filesystem::path> applicationCacheDirectory()
  {
    return applicationDirectory("XDG_CACHE_HOME", ".cache");
  }
} // namespace ao::utility
