// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/utility/AtomicFile.h>
#include <ao/utility/Log.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <vector>

namespace ao::utility
{
  namespace
  {
    Result<> createParentDirs(std::filesystem::path const& parentPath)
    {
      auto ec = std::error_code{};
      std::filesystem::create_directories(parentPath, ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to create directory {}: {}", parentPath.string(), ec.message()));
      }

      return {};
    }

    Result<int> createTempFile(std::filesystem::path const& parentPath, std::vector<char>& tempPath)
    {
      auto const tempTemplate = (parentPath / ".tmp.XXXXXX").string();
      tempPath = std::vector<char>(tempTemplate.begin(), tempTemplate.end());
      tempPath.push_back('\0');

      int const fd = ::mkstemp(tempPath.data());

      if (fd < 0)
      {
        return makeError(Error::Code::IoError, std::format("Failed to create temp file: {}", std::strerror(errno)));
      }

      return fd;
    }

    Result<> setTempPermissions(int fd, AtomicFilePermissions permissions)
    {
      if (permissions == AtomicFilePermissions::Default)
      {
        return {};
      }

      if (::fchmod(fd, static_cast<std::int32_t>(permissions)) != 0)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to set permissions on temp file: {}", std::strerror(errno)));
      }

      return {};
    }

    Result<> writeAll(int fd, std::string_view data)
    {
      std::size_t written = 0;

      while (written < data.size())
      {
        ssize_t const bytesWritten = ::write(fd, data.data() + written, data.size() - written);

        if (bytesWritten < 0)
        {
          if (errno == EINTR)
          {
            continue;
          }

          return makeError(Error::Code::IoError, std::format("Failed to write temp file: {}", std::strerror(errno)));
        }

        written += static_cast<std::size_t>(bytesWritten);
      }

      return {};
    }

    Result<> closeTempFile(int fd, std::vector<char> const& tempPath)
    {
      if (::fsync(fd) != 0)
      {
        APP_LOG_WARN("Failed to fsync temp file {}: {}", tempPath.data(), std::strerror(errno));
      }

      if (::close(fd) != 0)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to close temp file {}: {}", tempPath.data(), std::strerror(errno)));
      }

      return {};
    }

    Result<> renameTempFile(std::vector<char> const& tempPath, std::filesystem::path const& targetPath)
    {
      if (::rename(tempPath.data(), targetPath.c_str()) != 0)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Failed to rename temp file to {}: {}", targetPath.string(), std::strerror(errno)));
      }

      return {};
    }

    void syncParentDirectory(std::filesystem::path const& parentPath)
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      int const parentFd = ::open(parentPath.c_str(), O_RDONLY | O_DIRECTORY);

      if (parentFd < 0)
      {
        APP_LOG_WARN("Failed to open parent directory {} for fsync: {}", parentPath.string(), std::strerror(errno));
        return;
      }

      if (::fsync(parentFd) != 0)
      {
        APP_LOG_WARN("Failed to fsync parent directory {}: {}", parentPath.string(), std::strerror(errno));
      }

      ::close(parentFd);
    }
    struct [[nodiscard]] FdGuard final
    {
      int fd = -1;

      explicit FdGuard(std::int32_t fdValue)
        : fd{fdValue}
      {
      }

      ~FdGuard()
      {
        if (fd >= 0)
        {
          ::close(fd);
        }
      }

      FdGuard(FdGuard const&) = delete;
      FdGuard& operator=(FdGuard const&) = delete;
      FdGuard(FdGuard&&) = delete;
      FdGuard& operator=(FdGuard&&) = delete;
    };
  } // namespace

  Result<> writeAtomically(std::filesystem::path const& targetPath,
                           std::string_view data,
                           AtomicFilePermissions const permissions)
  {
    auto const parentPath = targetPath.parent_path();

    if (auto const result = createParentDirs(parentPath); !result)
    {
      return result;
    }

    auto tempPath = std::vector<char>{};
    auto fdResult = createTempFile(parentPath, tempPath);

    if (!fdResult)
    {
      return std::unexpected{fdResult.error()};
    }

    int const fd = *fdResult;
    auto guard = FdGuard{fd};

    if (auto const result = setTempPermissions(fd, permissions); !result)
    {
      return result;
    }

    if (auto const result = writeAll(fd, data); !result)
    {
      auto removeEc = std::error_code{};
      std::filesystem::remove(tempPath.data(), removeEc);
      return result;
    }

    guard.fd = -1; // Hand off ownership to closeTempFile

    if (auto const result = closeTempFile(fd, tempPath); !result)
    {
      auto removeEc = std::error_code{};
      std::filesystem::remove(tempPath.data(), removeEc);
      return result;
    }

    if (auto const result = renameTempFile(tempPath, targetPath); !result)
    {
      auto removeEc = std::error_code{};
      std::filesystem::remove(tempPath.data(), removeEc);
      return result;
    }

    syncParentDirectory(parentPath);
    return {};
  }
} // namespace ao::utility
