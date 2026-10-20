// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "LockFile.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace ao::library::detail
{
  namespace
  {
    constexpr std::size_t kSystemMessageBufferSize = 512;

    std::string systemMessage(DWORD errorCode)
    {
      auto buffer = std::array<char, kSystemMessageBufferSize>{};
      auto const size = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         errorCode,
                                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                         buffer.data(),
                                         static_cast<DWORD>(buffer.size()),
                                         nullptr);

      if (size == 0)
      {
        return std::format("Windows error {}", errorCode);
      }

      auto message = std::string{buffer.data(), size};

      while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
      {
        message.pop_back();
      }

      return message;
    }
  } // namespace

  struct LockFile::Impl final
  {
    explicit Impl(HANDLE handle) noexcept
      : handle{handle}
    {
    }

    ~Impl()
    {
      if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
      {
        std::ignore = ::CloseHandle(handle);
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    HANDLE handle = INVALID_HANDLE_VALUE;
  };

  Result<LockFile> LockFile::acquire(std::filesystem::path const& path, Mode const mode)
  {
    auto const pathText = utility::pathToUtf8(path);
    // Without delete sharing, Windows refuses to remove or rename the file
    // while any process holds it, as it does for LMDB's own files. A removed
    // file would let the next opener lock a new file that excludes nobody.
    auto* const handle = ::CreateFileW(path.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr,
                                       OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
      auto const errorCode = ::GetLastError();
      return makeError(
        Error::Code::IoError, std::format("Failed to open lock file '{}': {}", pathText, systemMessage(errorCode)));
    }

    auto implPtr = std::make_unique<Impl>(handle);
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;

    if (mode == Mode::Exclusive)
    {
      flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }

    if (auto overlapped = OVERLAPPED{}; ::LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped) == 0)
    {
      auto const errorCode = ::GetLastError();

      if (errorCode == ERROR_LOCK_VIOLATION || errorCode == ERROR_SHARING_VIOLATION)
      {
        return makeError(Error::Code::ResourceBusy, std::format("Lock file '{}' is held", pathText));
      }

      return makeError(
        Error::Code::IoError, std::format("Failed to lock '{}': {}", pathText, systemMessage(errorCode)));
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
