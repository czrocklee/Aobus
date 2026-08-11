// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/DetachedProcessLauncher.h>

#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

#ifdef _WIN32
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>
#endif

#include <filesystem>
#include <format>
#include <string>
#include <tuple>
#include <utility>

namespace ao::desktop
{
  namespace
  {
    namespace bp = boost::process::v2;

    template<typename Arguments, typename... Initializers>
    Result<> spawnDetached(DetachedProcessLaunch const& launch, Arguments&& arguments, Initializers&&... initializers)
    {
      auto ioContext = boost::asio::io_context{};
      auto launcher = bp::default_process_launcher{};
      auto error = boost::system::error_code{};
      auto const executable = boost::filesystem::path{launch.executable.native()};
      auto process = launcher(
        ioContext, error, executable, std::forward<Arguments>(arguments), std::forward<Initializers>(initializers)...);

      if (error)
      {
        return makeError(
          Error::Code::InitFailed,
          std::format(
            "Failed to launch detached process '{}': {}", utility::pathToUtf8(launch.executable), error.message()));
      }

      std::ignore = process.detach();
      return {};
    }

#ifdef _WIN32
    Result<std::wstring> wideArgument(std::string_view const argument)
    {
      if (argument.empty())
      {
        return std::wstring{};
      }

      if (argument.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
      {
        return makeError(Error::Code::ValueTooLarge, "A detached process argument is too long");
      }

      std::int32_t const inputLength = static_cast<std::int32_t>(argument.size());
      // MultiByteToWideChar consumes exactly inputLength UTF-8 bytes; null termination is not required.
      auto const required = ::MultiByteToWideChar(CP_UTF8,
                                                  MB_ERR_INVALID_CHARS,
                                                  argument.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                                  inputLength,
                                                  nullptr,
                                                  0);

      if (required == 0)
      {
        return makeError(Error::Code::InvalidInput, "A detached process argument is not valid UTF-8");
      }

      auto converted = std::wstring(static_cast<std::size_t>(required), L'\0');

      // The conversion call uses the same counted-input contract as the sizing call above.
      if (::MultiByteToWideChar(CP_UTF8,
                                MB_ERR_INVALID_CHARS,
                                argument.data(), // NOLINT(bugprone-suspicious-stringview-data-usage)
                                inputLength,
                                converted.data(),
                                required) == 0)
      {
        return makeError(Error::Code::InvalidInput, "A detached process argument is not valid UTF-8");
      }

      return converted;
    }

    void appendQuotedWindowsArgument(std::wstring& commandLine, std::wstring_view const argument)
    {
      commandLine.push_back(L'"');
      std::size_t backslashes = 0;

      for (auto const character : argument)
      {
        if (character == L'\\')
        {
          ++backslashes;
          continue;
        }

        if (character == L'"')
        {
          commandLine.append((backslashes * 2) + 1, L'\\');
          commandLine.push_back(character);
          backslashes = 0;
          continue;
        }

        commandLine.append(backslashes, L'\\');
        backslashes = 0;
        commandLine.push_back(character);
      }

      commandLine.append(backslashes * 2, L'\\');
      commandLine.push_back(L'"');
    }

    Result<std::wstring> windowsCommandLine(DetachedProcessLaunch const& launch)
    {
      auto commandLine = std::wstring{};
      appendQuotedWindowsArgument(commandLine, launch.executable.native());

      for (auto const& argument : launch.arguments)
      {
        auto convertedRes = wideArgument(argument);

        if (!convertedRes)
        {
          return std::unexpected{convertedRes.error()};
        }

        commandLine.push_back(L' ');
        appendQuotedWindowsArgument(commandLine, *convertedRes);
      }

      return commandLine;
    }
#endif
  } // namespace

  Result<> launchDetachedProcess(DetachedProcessLaunch const& launch)
  {
    if (launch.executable.empty())
    {
      return makeError(Error::Code::InvalidInput, "A detached process requires an executable");
    }

    auto const inheritStreams = launch.standardStreams == DetachedProcessStandardStreams::InheritParent;

#ifdef _WIN32
    auto commandLineRes = windowsCommandLine(launch);

    if (!commandLineRes)
    {
      return std::unexpected{commandLineRes.error()};
    }

    auto spawn = [&launch, &commandLineRes](auto&&... initializers)
    { return spawnDetached(launch, commandLineRes->c_str(), std::forward<decltype(initializers)>(initializers)...); };
#else
    auto spawn = [&launch](auto&&... initializers)
    { return spawnDetached(launch, launch.arguments, std::forward<decltype(initializers)>(initializers)...); };
#endif

    if (launch.optEnvironment)
    {
      auto environment = bp::process_environment{*launch.optEnvironment};

      if (inheritStreams)
      {
        return spawn(bp::process_stdio{}, environment);
      }

      return spawn(environment);
    }

    if (inheritStreams)
    {
      return spawn(bp::process_stdio{});
    }

    return spawn();
  }
} // namespace ao::desktop
