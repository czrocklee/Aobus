// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/utility/AtomicFile.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace ao::utility
{
  namespace
  {
    std::string systemMessage(DWORD const errorCode)
    {
      auto* rawBuffer = static_cast<char*>(nullptr);
      auto const size =
        ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr,
                         errorCode,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                         // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Allocation mode expects char**.
                         reinterpret_cast<char*>(&rawBuffer),
                         0,
                         nullptr);

      if (size == 0 || rawBuffer == nullptr)
      {
        return std::format("Windows error {}", errorCode);
      }

      auto message = std::string{rawBuffer, size};
      ::LocalFree(rawBuffer);

      while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
      {
        message.pop_back();
      }

      return message;
    }

    std::filesystem::path extendedPath(std::filesystem::path const& absolutePath)
    {
      auto const native = absolutePath.wstring();

      if (native.starts_with(L"\\\\?\\"))
      {
        return absolutePath;
      }

      if (native.starts_with(L"\\\\"))
      {
        return std::filesystem::path{L"\\\\?\\UNC\\" + native.substr(2)};
      }

      return std::filesystem::path{L"\\\\?\\" + native};
    }

    Result<std::filesystem::path> normalizedTargetPath(std::filesystem::path const& targetPath)
    {
      auto ec = std::error_code{};
      auto const absolute = std::filesystem::absolute(targetPath, ec);

      if (ec)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to resolve target path {}: {}", targetPath.string(), ec.message()));
      }

      return extendedPath(absolute);
    }

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

    struct [[nodiscard]] FileHandle final
    {
      HANDLE handle = INVALID_HANDLE_VALUE;

      explicit FileHandle(HANDLE value)
        : handle{value}
      {
      }

      ~FileHandle()
      {
        if (handle != INVALID_HANDLE_VALUE)
        {
          ::CloseHandle(handle);
        }
      }

      FileHandle(FileHandle const&) = delete;
      FileHandle& operator=(FileHandle const&) = delete;

      FileHandle(FileHandle&& other) noexcept
        : handle{std::exchange(other.handle, INVALID_HANDLE_VALUE)}
      {
      }

      FileHandle& operator=(FileHandle&& other) noexcept
      {
        if (this != &other)
        {
          if (handle != INVALID_HANDLE_VALUE)
          {
            ::CloseHandle(handle);
          }

          handle = std::exchange(other.handle, INVALID_HANDLE_VALUE);
        }

        return *this;
      }
    };

    struct TempFile final
    {
      std::filesystem::path path;
      FileHandle file;
    };

    Result<TempFile> createTempFile(std::filesystem::path const& parentPath)
    {
      static auto nextId = std::atomic<std::uint64_t>{0};
      constexpr std::uint32_t kMaxAttempts = 128;

      for (std::uint32_t attempt = 0; attempt < kMaxAttempts; ++attempt)
      {
        auto const id = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
        auto const fileName =
          std::format(".ao.tmp.{:08x}.{:016x}.{:016x}", ::GetCurrentProcessId(), ::GetTickCount64(), id);
        auto candidate = parentPath / fileName;
        auto* const handle = ::CreateFileW(
          candidate.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle != INVALID_HANDLE_VALUE)
        {
          return TempFile{.path = std::move(candidate), .file = FileHandle{handle}};
        }

        if (auto const errorCode = ::GetLastError();
            errorCode != ERROR_FILE_EXISTS && errorCode != ERROR_ALREADY_EXISTS)
        {
          return makeError(
            Error::Code::IoError, std::format("Failed to create temp file: {}", systemMessage(errorCode)));
        }
      }

      return makeError(Error::Code::IoError, "Failed to create a unique temp file after 128 attempts");
    }

    Result<> writeAll(HANDLE handle, std::string_view const data)
    {
      std::size_t written = 0;

      while (written < data.size())
      {
        auto const chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - written, 0x7ffff000ULL));
        DWORD bytesWritten = 0;

        if (::WriteFile(handle, data.data() + written, chunk, &bytesWritten, nullptr) == FALSE)
        {
          return makeError(
            Error::Code::IoError, std::format("Failed to write temp file: {}", systemMessage(::GetLastError())));
        }

        if (bytesWritten == 0)
        {
          return makeError(Error::Code::IoError, "Failed to write temp file: no bytes written");
        }

        written += bytesWritten;
      }

      return {};
    }

    Result<> flushFile(HANDLE handle)
    {
      if (::FlushFileBuffers(handle) == FALSE)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to flush temp file: {}", systemMessage(::GetLastError())));
      }

      return {};
    }

    Result<> closeFile(FileHandle& file, std::filesystem::path const& tempPath)
    {
      if (file.handle == INVALID_HANDLE_VALUE)
      {
        return {};
      }

      if (::CloseHandle(file.handle) == FALSE)
      {
        auto error = makeError(
          Error::Code::IoError,
          std::format("Failed to close temp file {}: {}", tempPath.string(), systemMessage(::GetLastError())));
        file.handle = INVALID_HANDLE_VALUE;
        return error;
      }

      file.handle = INVALID_HANDLE_VALUE;
      return {};
    }
  } // namespace

  Result<> writeAtomically(std::filesystem::path const& targetPath,
                           std::string_view const data,
                           AtomicFilePermissions const /*permissions*/)
  {
    auto normalizedTargetResult = normalizedTargetPath(targetPath);

    if (!normalizedTargetResult)
    {
      return std::unexpected{normalizedTargetResult.error()};
    }

    auto const& normalizedTarget = *normalizedTargetResult;
    auto const parentPath = normalizedTarget.parent_path();

    if (auto const result = createParentDirs(parentPath); !result)
    {
      return result;
    }

    auto tempFileResult = createTempFile(parentPath);

    if (!tempFileResult)
    {
      return std::unexpected{tempFileResult.error()};
    }

    auto tempFile = std::move(*tempFileResult);
    auto const& tempPath = tempFile.path;
    auto removeTemp = [&tempPath]
    {
      auto ec = std::error_code{};
      std::filesystem::remove(tempPath, ec);
    };

    if (auto const result = writeAll(tempFile.file.handle, data); !result)
    {
      std::ignore = closeFile(tempFile.file, tempPath);
      removeTemp();
      return result;
    }

    if (auto const result = flushFile(tempFile.file.handle); !result)
    {
      std::ignore = closeFile(tempFile.file, tempPath);
      removeTemp();
      return result;
    }

    if (auto const result = closeFile(tempFile.file, tempPath); !result)
    {
      removeTemp();
      return result;
    }

    if (::MoveFileExW(tempPath.wstring().c_str(),
                      normalizedTarget.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
    {
      auto error = makeError(
        Error::Code::IoError,
        std::format("Failed to rename temp file to {}: {}", targetPath.string(), systemMessage(::GetLastError())));
      removeTemp();
      return error;
    }

    return {};
  }
} // namespace ao::utility
