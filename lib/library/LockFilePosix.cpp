// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LockFile.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <sys/file.h>  // NOLINT(misc-include-cleaner) -- public flock header; Darwin forwards the declaration.
#include <sys/stat.h>  // NOLINT(misc-include-cleaner) -- public permission constants; Darwin uses private SDK headers.
#include <sys/types.h> // NOLINT(misc-include-cleaner) -- public mode_t header; Darwin uses a private SDK header.
#include <utility>

namespace ao::library::detail
{
  namespace
  {
    // Darwin's public headers forward this type and these macros from SDK-private headers.
    // NOLINTNEXTLINE(misc-include-cleaner)
    constexpr mode_t kLockFilePermissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;

    std::int32_t openLockFile(std::filesystem::path const& path)
    {
      while (true)
      {
        // open is variadic when O_CREAT supplies a permissions argument.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::int32_t const descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, kLockFilePermissions);

        if (descriptor >= 0 || errno != EINTR)
        {
          return descriptor;
        }
      }
    }

    std::int32_t tryLock(std::int32_t descriptor, LockFile::Mode const mode)
    {
      std::int32_t const operation = (mode == LockFile::Mode::Shared ? LOCK_SH : LOCK_EX) | LOCK_NB;

      while (true)
      {
        if (std::int32_t const result = ::flock(descriptor, operation); result == 0 || errno != EINTR)
        {
          return result;
        }
      }
    }
  } // namespace

  struct LockFile::Impl final
  {
    explicit Impl(std::int32_t descriptor) noexcept
      : descriptor{descriptor}
    {
    }

    ~Impl()
    {
      if (descriptor >= 0)
      {
        std::ignore = ::close(descriptor);
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::int32_t descriptor = -1;
  };

  Result<LockFile> LockFile::acquire(std::filesystem::path const& path, Mode const mode)
  {
    auto const pathText = utility::pathToUtf8(path);
    auto const descriptor = openLockFile(path);

    if (descriptor < 0)
    {
      auto const errorCode = errno;
      return makeError(
        Error::Code::IoError, std::format("Failed to open lock file '{}': {}", pathText, std::strerror(errorCode)));
    }

    auto implPtr = std::make_unique<Impl>(descriptor);

    if (tryLock(descriptor, mode) != 0)
    {
      auto const errorCode = errno;

      if (errorCode == EWOULDBLOCK || errorCode == EAGAIN)
      {
        return makeError(Error::Code::ResourceBusy, std::format("Lock file '{}' is held", pathText));
      }

      return makeError(
        Error::Code::IoError, std::format("Failed to lock '{}': {}", pathText, std::strerror(errorCode)));
    }

    return LockFile{std::move(implPtr)};
  }

  LockFile::LockFile(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  LockFile::~LockFile() = default;
  LockFile::LockFile(LockFile&&) noexcept = default;
  LockFile& LockFile::operator=(LockFile&&) noexcept = default;
} // namespace ao::library::detail
