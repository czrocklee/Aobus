// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WriterSessionLease.h"
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
    constexpr mode_t kWriterLeasePermissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;

    std::int32_t openLeaseFile(std::filesystem::path const& path)
    {
      while (true)
      {
        // open is variadic when O_CREAT supplies a permissions argument.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        std::int32_t const descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, kWriterLeasePermissions);

        if (descriptor >= 0 || errno != EINTR)
        {
          return descriptor;
        }
      }
    }

    std::int32_t tryLockLeaseFile(std::int32_t descriptor)
    {
      while (true)
      {
        std::int32_t const result = ::flock(descriptor, LOCK_EX | LOCK_NB);

        if (result == 0 || errno != EINTR)
        {
          return result;
        }
      }
    }
  } // namespace

  struct WriterSessionLease::Impl final
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

  Result<WriterSessionLease> WriterSessionLease::acquire(std::filesystem::path const& databasePath)
  {
    auto const leasePath = databasePath / utility::pathFromUtf8(kFileName);
    auto const descriptor = openLeaseFile(leasePath);

    if (descriptor < 0)
    {
      auto const errorCode = errno;
      return makeError(
        Error::Code::IoError,
        std::format("Failed to open library writer lease '{}': {}", leasePath.native(), std::strerror(errorCode)));
    }

    auto implPtr = std::make_unique<Impl>(descriptor);

    if (tryLockLeaseFile(descriptor) != 0)
    {
      auto const errorCode = errno;

      if (errorCode == EWOULDBLOCK || errorCode == EAGAIN)
      {
        return makeError(
          Error::Code::Conflict, std::format("Library already has a writable process: {}", databasePath.native()));
      }

      return makeError(
        Error::Code::IoError,
        std::format("Failed to acquire library writer lease '{}': {}", leasePath.native(), std::strerror(errorCode)));
    }

    return WriterSessionLease{std::move(implPtr)};
  }

  WriterSessionLease::WriterSessionLease(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  WriterSessionLease::~WriterSessionLease() = default;
  WriterSessionLease::WriterSessionLease(WriterSessionLease&&) noexcept = default;
  WriterSessionLease& WriterSessionLease::operator=(WriterSessionLease&&) noexcept = default;
} // namespace ao::library::detail
