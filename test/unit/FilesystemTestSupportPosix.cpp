// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FilesystemTestSupport.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::test
{
  namespace
  {
    bool isAccessDenied(std::error_code const& ec)
    {
      return ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted;
    }

    bool readRestrictionIsEffective(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      [[maybe_unused]] auto const iterator = std::filesystem::directory_iterator{path, ec};

      if (!ec)
      {
        return false;
      }

      if (isAccessDenied(ec))
      {
        return true;
      }

      throw std::system_error{ec, "failed to probe denied directory read access"};
    }

    bool writeRestrictionIsEffective(std::filesystem::path const& path)
    {
      auto const probeTemplate = (path / ".ao.access-probe.XXXXXX").string();
      auto probePath = std::vector<char>{probeTemplate.begin(), probeTemplate.end()};
      probePath.push_back('\0');

      if (auto const file = ::mkstemp(probePath.data()); file >= 0)
      {
        auto const closeResult = ::close(file);
        auto const closeError = errno;
        auto const unlinkResult = ::unlink(probePath.data());
        auto const unlinkError = errno;

        if (closeResult != 0)
        {
          throw std::system_error{closeError, std::generic_category(), "failed to close directory access probe"};
        }

        if (unlinkResult != 0)
        {
          throw std::system_error{unlinkError, std::generic_category(), "failed to remove directory access probe"};
        }

        return false;
      }

      if (errno == EACCES || errno == EPERM)
      {
        return true;
      }

      throw std::system_error{errno, std::generic_category(), "failed to probe denied directory write access"};
    }
  } // namespace

  struct ScopedDirectoryAccessGuard::Impl final
  {
    Impl(std::filesystem::path inputPath, DeniedDirectoryAccess inputAccess)
      : path{std::move(inputPath)}
    {
      auto ec = std::error_code{};
      originalPermissions = std::filesystem::status(path, ec).permissions();

      if (ec)
      {
        throw std::system_error{ec, "failed to inspect directory permissions"};
      }

      auto const deniedPermissions = inputAccess == DeniedDirectoryAccess::Read
                                       ? std::filesystem::perms::none
                                       : std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec;
      std::filesystem::permissions(path, deniedPermissions, std::filesystem::perm_options::replace, ec);

      if (ec)
      {
        throw std::system_error{ec, "failed to deny directory access"};
      }

      applied = true;

      try
      {
        restrictionEffective = inputAccess == DeniedDirectoryAccess::Read ? readRestrictionIsEffective(path)
                                                                          : writeRestrictionIsEffective(path);
      }
      catch (...)
      {
        restore();
        throw;
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() noexcept { restore(); }

    void restore() noexcept
    {
      if (!applied)
      {
        return;
      }

      auto ec = std::error_code{};
      std::filesystem::permissions(path, originalPermissions, std::filesystem::perm_options::replace, ec);
      applied = false;

      if (ec)
      {
        // NOLINTNEXTLINE(modernize-use-std-print): C I/O cannot throw from this noexcept cleanup path.
        std::fprintf(stderr, "Aobus test directory permission restore failed (error %d)\n", ec.value());
      }
    }

    std::filesystem::path path;
    std::filesystem::perms originalPermissions = std::filesystem::perms::unknown;
    bool applied = false;
    bool restrictionEffective = false;
  };

  ScopedDirectoryAccessGuard::ScopedDirectoryAccessGuard(std::filesystem::path path, DeniedDirectoryAccess access)
    : _implPtr{std::make_unique<Impl>(std::move(path), access)}
  {
  }

  ScopedDirectoryAccessGuard::~ScopedDirectoryAccessGuard() noexcept = default;

  bool ScopedDirectoryAccessGuard::effective() const noexcept
  {
    return _implPtr->restrictionEffective;
  }
} // namespace ao::test
