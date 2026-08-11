// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ao::test
{
  namespace
  {
    class [[nodiscard]] UniqueHandle final
    {
    public:
      UniqueHandle() = default;
      explicit UniqueHandle(HANDLE handle) noexcept
        : _handle{handle}
      {
      }
      UniqueHandle(UniqueHandle const&) = delete;
      UniqueHandle& operator=(UniqueHandle const&) = delete;

      UniqueHandle(UniqueHandle&& other) noexcept
        : _handle{other.release()}
      {
      }

      UniqueHandle& operator=(UniqueHandle&& other) noexcept
      {
        if (this != &other)
        {
          reset(other.release());
        }

        return *this;
      }

      ~UniqueHandle() noexcept { reset(); }

      HANDLE get() const noexcept { return _handle; }

      HANDLE release() noexcept
      {
        auto* const handle = _handle;
        _handle = nullptr;
        return handle;
      }

      void reset(HANDLE handle = nullptr) noexcept
      {
        if (_handle != nullptr && _handle != INVALID_HANDLE_VALUE)
        {
          [[maybe_unused]] auto const result = ::CloseHandle(_handle);
        }

        _handle = handle;
      }

    private:
      HANDLE _handle = nullptr;
    };

    std::filesystem::path readCurrentExecutablePath()
    {
      auto buffer = std::vector<wchar_t>(512);

      for (;;)
      {
        auto const written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (written == 0)
        {
          return {};
        }

        if (written < buffer.size() - 1)
        {
          return std::filesystem::path{std::wstring_view{buffer.data(), written}};
        }

        buffer.resize(buffer.size() * 2);
      }
    }

    std::string windowsError(std::string_view operation)
    {
      return std::string{operation} + " failed with Windows error " + std::to_string(::GetLastError());
    }

    bool createCapturedPipe(SECURITY_ATTRIBUTES& securityAttributes,
                            UniqueHandle& readHandle,
                            UniqueHandle& writeHandle,
                            std::string& launchError)
    {
      auto* rawReadHandle = HANDLE{};
      auto* rawWriteHandle = HANDLE{};

      if (::CreatePipe(&rawReadHandle, &rawWriteHandle, &securityAttributes, 0) == 0)
      {
        launchError = windowsError("CreatePipe");
        return false;
      }

      readHandle.reset(rawReadHandle);
      writeHandle.reset(rawWriteHandle);

      if (::SetHandleInformation(readHandle.get(), HANDLE_FLAG_INHERIT, 0) == 0)
      {
        launchError = windowsError("SetHandleInformation");
        return false;
      }

      return true;
    }

    void captureOutput(HANDLE readHandle, std::string& output)
    {
      auto buffer = std::array<char, 4096>{};

      for (;;)
      {
        DWORD readSize = 0;

        if (::ReadFile(readHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &readSize, nullptr) == 0 ||
            readSize == 0)
        {
          break;
        }

        auto const available = kMaximumProbeOutputBytes - output.size();
        auto const capturedSize = std::min<std::size_t>(readSize, available);
        output.append(buffer.data(), capturedSize);
      }
    }
  } // namespace

  std::filesystem::path currentProbeExecutablePath()
  {
    return readCurrentExecutablePath();
  }

  std::filesystem::path fatalProbeExecutablePath()
  {
    return siblingProbeExecutablePath("ao_fatal_probe");
  }

  std::filesystem::path siblingProbeExecutablePath(std::string_view const executableStem)
  {
    auto executablePath = currentProbeExecutablePath();

    if (!executablePath.empty())
    {
      auto executableName = std::wstring{executableStem.begin(), executableStem.end()};
      executableName += L".exe";
      executablePath.replace_filename(executableName);
    }

    return executablePath;
  }

  ProbeProcessResult runProbeProcess(std::filesystem::path const& executablePath,
                                     std::string_view scenarioName,
                                     std::chrono::milliseconds timeout)
  {
    auto result = ProbeProcessResult{};

    if (executablePath.empty())
    {
      result.launchError = windowsError("GetModuleFileNameW");
      return result;
    }

    auto securityAttributes = SECURITY_ATTRIBUTES{
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    auto standardOutputReadHandle = UniqueHandle{};
    auto standardOutputWriteHandle = UniqueHandle{};
    auto standardErrorReadHandle = UniqueHandle{};
    auto standardErrorWriteHandle = UniqueHandle{};

    if (!createCapturedPipe(
          securityAttributes, standardOutputReadHandle, standardOutputWriteHandle, result.launchError) ||
        !createCapturedPipe(securityAttributes, standardErrorReadHandle, standardErrorWriteHandle, result.launchError))
    {
      return result;
    }

    auto nullHandle = UniqueHandle{::CreateFileW(L"NUL",
                                                 GENERIC_READ | GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                 &securityAttributes,
                                                 OPEN_EXISTING,
                                                 FILE_ATTRIBUTE_NORMAL,
                                                 nullptr)};

    if (nullHandle.get() == INVALID_HANDLE_VALUE)
    {
      result.launchError = windowsError("CreateFileW(NUL)");
      return result;
    }

    auto wideTestName = std::wstring{scenarioName.begin(), scenarioName.end()};
    auto commandLine = L"\"" + executablePath.native() + L"\" --aobus-probe-child \"" + wideTestName + L"\"";
    auto mutableCommandLine = std::vector<wchar_t>{commandLine.begin(), commandLine.end()};
    mutableCommandLine.push_back(L'\0');

    auto startupInfo = STARTUPINFOW{};
    startupInfo.cb = sizeof(STARTUPINFOW);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nullHandle.get();
    startupInfo.hStdOutput = standardOutputWriteHandle.get();
    startupInfo.hStdError = standardErrorWriteHandle.get();
    auto processInfo = PROCESS_INFORMATION{};

    if (::CreateProcessW(executablePath.c_str(),
                         mutableCommandLine.data(),
                         nullptr,
                         nullptr,
                         TRUE,
                         CREATE_NO_WINDOW,
                         nullptr,
                         nullptr,
                         &startupInfo,
                         &processInfo) == 0)
    {
      result.launchError = windowsError("CreateProcessW");
      return result;
    }

    result.started = true;
    auto processHandle = UniqueHandle{processInfo.hProcess};
    auto threadHandle = UniqueHandle{processInfo.hThread};
    standardOutputWriteHandle.reset();
    standardErrorWriteHandle.reset();
    nullHandle.reset();
    auto standardOutputReader = std::jthread{[handle = standardOutputReadHandle.get(), &result]
                                             { captureOutput(handle, result.standardOutput); }};
    auto standardErrorReader =
      std::jthread{[handle = standardErrorReadHandle.get(), &result] { captureOutput(handle, result.standardError); }};

    auto const boundedTimeout =
      std::clamp<std::int64_t>(timeout.count(), 0, static_cast<std::int64_t>(std::numeric_limits<DWORD>::max() - 1));
    auto const waitResult = ::WaitForSingleObject(processHandle.get(), static_cast<DWORD>(boundedTimeout));

    if (waitResult == WAIT_TIMEOUT)
    {
      result.timedOut = true;
      [[maybe_unused]] auto const terminateResult = ::TerminateProcess(processHandle.get(), 1);
      [[maybe_unused]] auto const finalWait = ::WaitForSingleObject(processHandle.get(), INFINITE);
    }
    else if (waitResult == WAIT_FAILED)
    {
      result.launchError = windowsError("WaitForSingleObject");
      [[maybe_unused]] auto const terminateResult = ::TerminateProcess(processHandle.get(), 1);
      [[maybe_unused]] auto const finalWait = ::WaitForSingleObject(processHandle.get(), INFINITE);
    }

    if (DWORD exitCode = 0; ::GetExitCodeProcess(processHandle.get(), &exitCode) != 0)
    {
      result.exited = true;
      result.exitCode = exitCode;
    }
    else if (result.launchError.empty())
    {
      result.launchError = windowsError("GetExitCodeProcess");
    }

    standardOutputReader.join();
    standardErrorReader.join();

    return result;
  }
} // namespace ao::test
