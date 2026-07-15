// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AtomicFileTransaction.h"
#include <ao/utility/AtomicFile.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <sddl.h>
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
#include <vector>

namespace ao::utility
{
  namespace
  {
    class LocalAllocation final
    {
    public:
      explicit LocalAllocation(HLOCAL value)
        : _value{value}
      {
      }

      ~LocalAllocation() noexcept
      {
        if (_value != nullptr)
        {
          std::ignore = ::LocalFree(_value);
        }
      }

      LocalAllocation(LocalAllocation const&) = delete;
      LocalAllocation& operator=(LocalAllocation const&) = delete;

      LocalAllocation(LocalAllocation&& other) noexcept
        : _value{std::exchange(other._value, nullptr)}
      {
      }

      LocalAllocation& operator=(LocalAllocation&&) = delete;

      HLOCAL get() const noexcept { return _value; }

    private:
      HLOCAL _value = nullptr;
    };

    class KernelHandle final
    {
    public:
      explicit KernelHandle(HANDLE value)
        : _value{value}
      {
      }

      ~KernelHandle() noexcept { closeBestEffort(); }

      KernelHandle(KernelHandle const&) = delete;
      KernelHandle& operator=(KernelHandle const&) = delete;

      KernelHandle(KernelHandle&& other) noexcept
        : _value{std::exchange(other._value, INVALID_HANDLE_VALUE)}
      {
      }

      KernelHandle& operator=(KernelHandle&&) = delete;

      HANDLE get() const noexcept { return _value; }
      HANDLE release() noexcept { return std::exchange(_value, INVALID_HANDLE_VALUE); }

      void closeBestEffort() noexcept
      {
        if (_value != nullptr && _value != INVALID_HANDLE_VALUE)
        {
          HANDLE const handle = release();
          std::ignore = ::CloseHandle(handle);
        }
      }

    private:
      HANDLE _value = INVALID_HANDLE_VALUE;
    };

    std::string systemMessage(DWORD const errorCode)
    {
      char* rawBuffer = nullptr;
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

      auto allocation = LocalAllocation{rawBuffer};
      auto message = std::string{rawBuffer, size};

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

    Result<std::wstring> currentUserSidString()
    {
      HANDLE rawToken = nullptr;

      if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE)
      {
        auto const errorCode = ::GetLastError();
        return makeError(
          Error::Code::IoError, std::format("Failed to open process token: {}", systemMessage(errorCode)));
      }

      auto token = KernelHandle{rawToken};
      DWORD tokenInfoSize = 0;
      auto const firstQuery = ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &tokenInfoSize);

      if (firstQuery != FALSE || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      {
        auto const errorCode = ::GetLastError();
        return makeError(
          Error::Code::IoError, std::format("Failed to size process token information: {}", systemMessage(errorCode)));
      }

      auto tokenInfo = std::vector<std::byte>(tokenInfoSize);

      if (::GetTokenInformation(token.get(), TokenUser, tokenInfo.data(), tokenInfoSize, &tokenInfoSize) == FALSE)
      {
        auto const errorCode = ::GetLastError();
        return makeError(
          Error::Code::IoError, std::format("Failed to read process token information: {}", systemMessage(errorCode)));
      }

      auto const* tokenUser = static_cast<TOKEN_USER const*>(static_cast<void const*>(tokenInfo.data()));
      LPWSTR rawSidString = nullptr;

      if (::ConvertSidToStringSidW(tokenUser->User.Sid, &rawSidString) == FALSE)
      {
        auto const errorCode = ::GetLastError();
        return makeError(
          Error::Code::IoError, std::format("Failed to encode process SID: {}", systemMessage(errorCode)));
      }

      auto sidAllocation = LocalAllocation{rawSidString};
      return std::wstring{rawSidString};
    }

    Result<LocalAllocation> createPrivateSecurityDescriptor()
    {
      auto sidResult = currentUserSidString();

      if (!sidResult)
      {
        return std::unexpected{sidResult.error()};
      }

      auto descriptorText = std::wstring{L"D:P(A;;FA;;;SY)(A;;FA;;;"};
      descriptorText += *sidResult;
      descriptorText += L")";

      PSECURITY_DESCRIPTOR rawDescriptor = nullptr;

      if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptorText.c_str(), SDDL_REVISION_1, &rawDescriptor, nullptr) == FALSE)
      {
        auto const errorCode = ::GetLastError();
        return makeError(
          Error::Code::IoError, std::format("Failed to create private file ACL: {}", systemMessage(errorCode)));
      }

      return LocalAllocation{rawDescriptor};
    }

    class WindowsTemporaryFile final
    {
    public:
      WindowsTemporaryFile(std::filesystem::path path, HANDLE handle)
        : _path{std::move(path)}, _file{handle}
      {
      }

      ~WindowsTemporaryFile() noexcept
      {
        _file.closeBestEffort();

        if (!_path.empty())
        {
          std::ignore = ::DeleteFileW(_path.c_str());
        }
      }

      WindowsTemporaryFile(WindowsTemporaryFile const&) = delete;
      WindowsTemporaryFile& operator=(WindowsTemporaryFile const&) = delete;

      WindowsTemporaryFile(WindowsTemporaryFile&& other) noexcept
        : _path{std::move(other._path)}, _file{std::move(other._file)}
      {
        other._path.clear();
      }

      WindowsTemporaryFile& operator=(WindowsTemporaryFile&&) = delete;

      Result<> writeAll(std::string_view data) const
      {
        std::size_t written = 0;

        while (written < data.size())
        {
          auto const chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - written, 0x7ffff000ULL));
          DWORD bytesWritten = 0;

          if (::WriteFile(_file.get(), data.data() + written, chunk, &bytesWritten, nullptr) == FALSE)
          {
            auto const errorCode = ::GetLastError();
            return makeError(
              Error::Code::IoError, std::format("Failed to write temp file: {}", systemMessage(errorCode)));
          }

          if (bytesWritten == 0)
          {
            return makeError(Error::Code::IoError, "Failed to write temp file: no bytes written");
          }

          written += bytesWritten;
        }

        return {};
      }

      Result<> synchronizeData() const
      {
        if (::FlushFileBuffers(_file.get()) == FALSE)
        {
          auto const errorCode = ::GetLastError();
          return makeError(
            Error::Code::IoError, std::format("Failed to flush temp file: {}", systemMessage(errorCode)));
        }

        return {};
      }

      Result<> closeForReplacement()
      {
        HANDLE const handle = _file.release();

        if (::CloseHandle(handle) == FALSE)
        {
          auto const errorCode = ::GetLastError();
          return makeError(Error::Code::IoError,
                           std::format("Failed to close temp file {}: {}", _path.string(), systemMessage(errorCode)));
        }

        return {};
      }

      Result<> replaceTarget(std::filesystem::path const& targetPath)
      {
        if (::MoveFileExW(_path.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
            FALSE)
        {
          auto const errorCode = ::GetLastError();
          return makeError(
            Error::Code::IoError,
            std::format("Failed to rename temp file to {}: {}", targetPath.string(), systemMessage(errorCode)));
        }

        _path.clear();
        return {};
      }

    private:
      std::filesystem::path _path;
      KernelHandle _file;
    };

    class WindowsAtomicFileOperations final
    {
    public:
      Result<std::filesystem::path> normalizeTargetPath(std::filesystem::path const& targetPath) const
      {
        auto ec = std::error_code{};
        auto const absolute = std::filesystem::absolute(targetPath, ec);

        if (ec)
        {
          return makeError(Error::Code::IoError,
                           std::format("Failed to resolve target path {}: {}", targetPath.string(), ec.message()));
        }

        return extendedPath(absolute);
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

      Result<WindowsTemporaryFile> createPrivateTemporaryFile(std::filesystem::path const& parentPath) const
      {
        auto descriptorResult = createPrivateSecurityDescriptor();

        if (!descriptorResult)
        {
          return std::unexpected{descriptorResult.error()};
        }

        auto descriptor = std::move(*descriptorResult);
        auto securityAttributes = SECURITY_ATTRIBUTES{
          .nLength = static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)),
          .lpSecurityDescriptor = descriptor.get(),
          .bInheritHandle = FALSE,
        };
        static auto nextId = std::atomic<std::uint64_t>{0};
        constexpr std::uint32_t kMaxAttempts = 128;

        for (std::uint32_t attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
          auto const id = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
          auto const fileName =
            std::format(L".ao.tmp.{:08x}.{:016x}.{:016x}", ::GetCurrentProcessId(), ::GetTickCount64(), id);
          auto candidate = parentPath / fileName;
          HANDLE const handle = ::CreateFileW(
            candidate.c_str(), GENERIC_WRITE, 0, &securityAttributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

          if (handle != INVALID_HANDLE_VALUE)
          {
            return WindowsTemporaryFile{std::move(candidate), handle};
          }

          auto const errorCode = ::GetLastError();

          if (errorCode != ERROR_FILE_EXISTS && errorCode != ERROR_ALREADY_EXISTS)
          {
            return makeError(
              Error::Code::IoError, std::format("Failed to create temp file: {}", systemMessage(errorCode)));
          }
        }

        return makeError(Error::Code::IoError, "Failed to create a unique temp file after 128 attempts");
      }

      void synchronizeParentDirectoryBestEffort(std::filesystem::path const& /*parentPath*/) const noexcept
      {
        // MoveFileExW's write-through request is the complete documented Windows
        // sequence; there is no separate directory-handle barrier here.
      }
    };
  } // namespace

  Result<> writeAtomically(std::filesystem::path const& targetPath, std::string_view data)
  {
    auto operations = WindowsAtomicFileOperations{};
    return detail::runAtomicReplacement(operations, targetPath, data);
  }
} // namespace ao::utility
