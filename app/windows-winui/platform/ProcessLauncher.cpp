// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/ProcessLauncher.h"

#include "pch.h"
#include <ao/Error.h>
#include <ao/winui/app/CommandLine.h>
#include <ao/winui/app/StartupOptions.h>

#include <gsl-lite/gsl-lite.hpp>
#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::winui
{
  namespace
  {
    std::string windowsErrorMessage(DWORD const code)
    {
      return std::system_category().message(static_cast<std::int32_t>(code));
    }

    Result<std::string> utf8FromWide(std::wstring_view const value)
    {
      if (value.empty())
      {
        return std::string{};
      }

      if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      {
        return makeError(Error::Code::ValueTooLarge, "A WinUI startup argument is too long");
      }

      std::int32_t const inputLength = static_cast<std::int32_t>(value.size());
      // WideCharToMultiByte consumes exactly inputLength UTF-16 code units; null termination is not required.
      auto const required = ::WideCharToMultiByte(CP_UTF8,
                                                  WC_ERR_INVALID_CHARS,
                                                  value.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                                  inputLength,
                                                  nullptr,
                                                  0,
                                                  nullptr,
                                                  nullptr);

      if (required == 0)
      {
        auto const code = ::GetLastError();
        return makeError(Error::Code::InvalidInput,
                         std::format("Failed to decode a WinUI startup argument: {}", windowsErrorMessage(code)));
      }

      auto output = std::string(static_cast<std::size_t>(required), '\0');

      // The conversion call uses the same counted-input contract as the sizing call above.
      if (::WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                value.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                inputLength,
                                output.data(),
                                required,
                                nullptr,
                                nullptr) == 0)
      {
        auto const code = ::GetLastError();
        return makeError(Error::Code::InvalidInput,
                         std::format("Failed to decode a WinUI startup argument: {}", windowsErrorMessage(code)));
      }

      return output;
    }

    Result<std::filesystem::path> currentExecutablePath()
    {
      // A long-path-aware executable is not bounded by MAX_PATH.
      auto buffer = std::wstring(MAX_PATH, L'\0');

      while (true)
      {
        auto const written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (written == 0)
        {
          auto const code = ::GetLastError();
          return makeError(Error::Code::NotFound,
                           std::format("Failed to locate the running Aobus executable: {}", windowsErrorMessage(code)));
        }

        if (written < buffer.size())
        {
          buffer.resize(written);
          return std::filesystem::path{buffer};
        }

        if (buffer.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) / 2)
        {
          return makeError(Error::Code::ValueTooLarge, "The running Aobus executable path is too long");
        }

        buffer.resize(buffer.size() * 2);
      }
    }
  } // namespace

  Result<StartupOptions> readStartupOptions()
  {
    std::int32_t argumentCount = 0;
    auto** const arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);

    if (arguments == nullptr || argumentCount < 1)
    {
      auto const code = ::GetLastError();
      return makeError(Error::Code::InvalidInput,
                       std::format("Failed to parse the WinUI process command line: {}", windowsErrorMessage(code)));
    }

    auto const releaseArguments =
      gsl_lite::finally([arguments] { ::LocalFree(static_cast<HLOCAL>(static_cast<void*>(arguments))); });
    auto storage = std::vector<std::string>{};
    storage.reserve(static_cast<std::size_t>(argumentCount - 1));

    for (std::int32_t index = 1; index < argumentCount; ++index)
    {
      auto converted = utf8FromWide(arguments[index]);

      if (!converted)
      {
        return std::unexpected{converted.error()};
      }

      storage.push_back(std::move(*converted));
    }

    auto views = std::vector<std::string_view>{};
    views.reserve(storage.size());

    for (auto const& argument : storage)
    {
      views.emplace_back(argument);
    }

    return parseStartupOptions(std::span<std::string_view const>{views});
  }

  Result<> launchLibraryProcess(std::filesystem::path const& libraryRoot)
  {
    if (libraryRoot.empty())
    {
      return makeError(Error::Code::InvalidInput, "A successor process requires a library root");
    }

    auto executable = currentExecutablePath();

    if (!executable)
    {
      return std::unexpected{executable.error()};
    }

    auto commandLine = quoteCommandLineArgument(executable->native());
    commandLine.push_back(L' ');
    commandLine += quoteCommandLineArgument(L"--library-root");
    commandLine.push_back(L' ');
    commandLine += quoteCommandLineArgument(libraryRoot.native());
    auto mutableCommandLine = std::vector<wchar_t>{commandLine.begin(), commandLine.end()};
    mutableCommandLine.push_back(L'\0');
    auto startup = STARTUPINFOW{};
    startup.cb = sizeof(startup);
    auto process = PROCESS_INFORMATION{};

    if (::CreateProcessW(executable->c_str(),
                         mutableCommandLine.data(),
                         nullptr,
                         nullptr,
                         FALSE,
                         0,
                         nullptr,
                         nullptr,
                         &startup,
                         &process) == FALSE)
    {
      auto const code = ::GetLastError();
      return makeError(Error::Code::InitFailed,
                       std::format("Failed to launch the successor Aobus process: {}", windowsErrorMessage(code)));
    }

    std::ignore = ::CloseHandle(process.hThread);
    std::ignore = ::CloseHandle(process.hProcess);
    return {};
  }
} // namespace ao::winui
