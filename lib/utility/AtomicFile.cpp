// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AtomicFileTransaction.h"
#include <ao/Error.h>
#include <ao/utility/AtomicFile.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::utility
{
  namespace
  {
    class PosixTemporaryFile final
    {
    public:
      PosixTemporaryFile(std::vector<char> path, std::int32_t fd)
        : _path{std::move(path)}, _fd{fd}
      {
      }

      ~PosixTemporaryFile() noexcept
      {
        if (_fd >= 0)
        {
          std::ignore = ::close(_fd);
        }

        if (!_path.empty())
        {
          std::ignore = ::unlink(_path.data());
        }
      }

      PosixTemporaryFile(PosixTemporaryFile const&) = delete;
      PosixTemporaryFile& operator=(PosixTemporaryFile const&) = delete;

      PosixTemporaryFile(PosixTemporaryFile&& other) noexcept
        : _path{std::move(other._path)}, _fd{std::exchange(other._fd, -1)}
      {
        other._path.clear();
      }

      PosixTemporaryFile& operator=(PosixTemporaryFile&&) = delete;

      std::int32_t nativeHandle() const noexcept { return _fd; }

      Result<> writeAll(std::string_view data) const
      {
        std::size_t written = 0;

        while (written < data.size())
        {
          ssize_t const bytesWritten = ::write(_fd, data.data() + written, data.size() - written);

          if (bytesWritten < 0)
          {
            if (errno == EINTR)
            {
              continue;
            }

            return makeError(Error::Code::IoError, std::format("Failed to write temp file: {}", std::strerror(errno)));
          }

          if (bytesWritten == 0)
          {
            return makeError(Error::Code::IoError, "Failed to write temp file: no bytes written");
          }

          written += static_cast<std::size_t>(bytesWritten);
        }

        return {};
      }

      Result<> synchronizeData() const
      {
        if (::fsync(_fd) != 0)
        {
          return makeError(
            Error::Code::IoError, std::format("Failed to fsync temp file {}: {}", _path.data(), std::strerror(errno)));
        }

        return {};
      }

      Result<> closeForReplacement()
      {
        if (std::int32_t const fd = std::exchange(_fd, -1); ::close(fd) != 0)
        {
          return makeError(
            Error::Code::IoError, std::format("Failed to close temp file {}: {}", _path.data(), std::strerror(errno)));
        }

        return {};
      }

      Result<> replaceTarget(std::filesystem::path const& targetPath)
      {
        if (::rename(_path.data(), targetPath.c_str()) != 0)
        {
          return makeError(
            Error::Code::IoError,
            std::format("Failed to rename temp file to {}: {}", targetPath.string(), std::strerror(errno)));
        }

        _path.clear();
        return {};
      }

    private:
      std::vector<char> _path;
      std::int32_t _fd = -1;
    };

    class PosixAtomicFileOperations final
    {
    public:
      Result<std::filesystem::path> normalizeTargetPath(std::filesystem::path const& targetPath) const
      {
        return targetPath;
      }

      Result<> createParentDirectories(std::filesystem::path const& parentPath) const
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

      Result<PosixTemporaryFile> createPrivateTemporaryFile(std::filesystem::path const& parentPath) const
      {
        auto const tempTemplate = (parentPath / ".temp.XXXXXX").string();
        auto tempPath = std::vector<char>(tempTemplate.begin(), tempTemplate.end());
        tempPath.push_back('\0');

        std::int32_t const fd = ::mkstemp(tempPath.data());

        if (fd < 0)
        {
          return makeError(Error::Code::IoError, std::format("Failed to create temp file: {}", std::strerror(errno)));
        }

        auto temporaryFile = PosixTemporaryFile{std::move(tempPath), fd};

        if (::fchmod(temporaryFile.nativeHandle(), S_IRUSR | S_IWUSR) != 0)
        {
          return makeError(
            Error::Code::IoError, std::format("Failed to set permissions on temp file: {}", std::strerror(errno)));
        }

        return temporaryFile;
      }

      void synchronizeParentDirectoryBestEffort(std::filesystem::path const& parentPath) const noexcept
      {
        // The replacement is already visible. A directory-barrier failure must
        // not be reported as a conventional "not applied" error.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::int32_t const parentFd = ::open(parentPath.c_str(), O_RDONLY | O_DIRECTORY);

        if (parentFd < 0)
        {
          return;
        }

        std::ignore = ::fsync(parentFd);
        std::ignore = ::close(parentFd);
      }
    };
  } // namespace

  Result<> writeAtomically(std::filesystem::path const& targetPath, std::string_view data)
  {
    auto operations = PosixAtomicFileOperations{};
    return detail::runAtomicReplacement(operations, targetPath, data);
  }
} // namespace ao::utility
