// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/ProcessLauncher.h"

#include "pch.h"
#include <ao/Error.h>
#include <ao/desktop/DetachedProcessLauncher.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/desktop/LibrarySwitch.h>

#include <gsl-lite/gsl-lite.hpp>
#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

  Result<std::optional<desktop::LibrarySwitchRequest>> readLibrarySuccessorRequest()
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
      auto convertedRes = utf8FromWide(arguments[index]);

      if (!convertedRes)
      {
        return std::unexpected{convertedRes.error()};
      }

      storage.push_back(std::move(*convertedRes));
    }

    auto views = std::vector<std::string_view>{};
    views.reserve(storage.size());

    for (auto const& argument : storage)
    {
      views.emplace_back(argument);
    }

    auto parsedRes = desktop::parseLibrarySuccessorProtocol(std::span<std::string_view const>{views});

    if (!parsedRes)
    {
      return std::unexpected{parsedRes.error()};
    }

    if (!parsedRes->remainingArguments.empty())
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("Unknown WinUI startup argument '{}'", parsedRes->remainingArguments.front()));
    }

    return std::move(parsedRes->optRequest);
  }

  Result<> launchLibraryProcess(desktop::LibrarySwitchRequest const& request)
  {
    auto argumentsRes = desktop::librarySuccessorArguments(request);

    if (!argumentsRes)
    {
      return std::unexpected{argumentsRes.error()};
    }

    auto executableRes = currentExecutablePath();

    if (!executableRes)
    {
      return std::unexpected{executableRes.error()};
    }

    auto launchedRes =
      desktop::launchDetachedProcess({.executable = std::move(*executableRes), .arguments = std::move(*argumentsRes)});

    if (!launchedRes)
    {
      return makeError(launchedRes.error().code,
                       std::format("Failed to launch the successor Aobus process: {}", launchedRes.error().message));
    }

    return {};
  }
} // namespace ao::winui
