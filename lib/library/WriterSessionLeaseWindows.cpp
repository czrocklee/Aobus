// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "WriterSessionLease.h"
#include <ao/Error.h>

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
    constexpr auto kWriterLeaseFileName = L".aobus-writer.lock";
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

  struct WriterSessionLease::Impl final
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

  Result<WriterSessionLease> WriterSessionLease::acquire(std::filesystem::path const& databasePath)
  {
    auto const leasePath = databasePath / kWriterLeaseFileName;
    auto* const handle = ::CreateFileW(leasePath.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr);

    if (handle == INVALID_HANDLE_VALUE)
    {
      auto const errorCode = ::GetLastError();
      return makeError(
        Error::Code::IoError, std::format("Failed to open library writer lease: {}", systemMessage(errorCode)));
    }

    auto implPtr = std::make_unique<Impl>(handle);

    if (auto overlapped = OVERLAPPED{};
        ::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped) ==
        0)
    {
      auto const errorCode = ::GetLastError();

      if (errorCode == ERROR_LOCK_VIOLATION || errorCode == ERROR_SHARING_VIOLATION)
      {
        return makeError(Error::Code::Conflict, "Library already has a writable process");
      }

      return makeError(
        Error::Code::IoError, std::format("Failed to acquire library writer lease: {}", systemMessage(errorCode)));
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
