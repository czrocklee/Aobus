// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/LibrarySuccessorProtocol.h>

#include <ao/Error.h>
#include <ao/desktop/LibraryPath.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/utility/Path.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::desktop
{
  namespace
  {
    constexpr auto kLibraryRootPrefix = "--library-root=";

    Result<std::filesystem::path> parseAbsoluteRoot(std::string_view const value)
    {
      if (value.empty())
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Startup argument '{}' requires a path", kLibraryRootOption));
      }

      try
      {
        auto root = utility::pathFromUtf8(value);

        if (!root.is_absolute())
        {
          return makeError(Error::Code::InvalidInput,
                           std::format("Startup argument '{}' requires an absolute path", kLibraryRootOption));
        }

        return normalizeLibraryRoot(std::move(root));
      }
      catch (std::filesystem::filesystem_error const& error)
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Invalid UTF-8 library root '{}': {}", value, error.what()));
      }
    }
  } // namespace

  Result<LibrarySuccessorProtocolParse> parseLibrarySuccessorProtocol(std::span<std::string_view const> const arguments)
  {
    auto remaining = std::vector<std::string>{};
    remaining.reserve(arguments.size());
    auto optRoot = std::optional<std::filesystem::path>{};
    bool successor = false;
    bool scanAfterOpen = false;

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
      auto const argument = arguments[index];

      if (argument == kLibrarySuccessorOption)
      {
        if (successor)
        {
          return makeError(Error::Code::InvalidInput,
                           std::format("Startup argument '{}' may only be specified once", kLibrarySuccessorOption));
        }

        successor = true;
        continue;
      }

      auto optRootValue = std::optional<std::string_view>{};

      if (argument == kLibraryRootOption)
      {
        if (index + 1 >= arguments.size() || arguments[index + 1].empty() || arguments[index + 1].starts_with('-'))
        {
          return makeError(
            Error::Code::InvalidInput, std::format("Startup argument '{}' requires a path", kLibraryRootOption));
        }

        optRootValue = arguments[++index];
      }
      else if (argument.starts_with(kLibraryRootPrefix))
      {
        optRootValue = argument.substr(std::string_view{kLibraryRootPrefix}.size());
      }

      if (optRootValue)
      {
        if (optRoot)
        {
          return makeError(Error::Code::InvalidInput,
                           std::format("Startup argument '{}' may only be specified once", kLibraryRootOption));
        }

        auto rootRes = parseAbsoluteRoot(*optRootValue);

        if (!rootRes)
        {
          return std::unexpected{rootRes.error()};
        }

        optRoot = std::move(*rootRes);
        continue;
      }

      if (argument == kScanAfterOpenOption)
      {
        if (scanAfterOpen)
        {
          return makeError(Error::Code::InvalidInput,
                           std::format("Startup argument '{}' may only be specified once", kScanAfterOpenOption));
        }

        scanAfterOpen = true;
        continue;
      }

      remaining.emplace_back(argument);
    }

    if (successor != optRoot.has_value())
    {
      return makeError(
        Error::Code::InvalidInput,
        std::format(
          "Successor arguments '{}' and '{}' must be specified together", kLibrarySuccessorOption, kLibraryRootOption));
    }

    if (scanAfterOpen && !successor)
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("Startup argument '{}' requires a successor startup", kScanAfterOpenOption));
    }

    auto parsed = LibrarySuccessorProtocolParse{.optRequest = std::nullopt, .remainingArguments = std::move(remaining)};

    if (optRoot)
    {
      parsed.optRequest = LibrarySwitchRequest{.libraryRoot = std::move(*optRoot), .scanAfterOpen = scanAfterOpen};
    }

    return parsed;
  }

  Result<std::vector<std::string>> librarySuccessorArguments(LibrarySwitchRequest const& request)
  {
    if (request.libraryRoot.empty() || !request.libraryRoot.is_absolute())
    {
      return makeError(Error::Code::InvalidInput, "A successor process requires an absolute library root");
    }

    auto arguments = std::vector{std::string{kLibrarySuccessorOption},
                                 std::string{kLibraryRootOption},
                                 utility::pathToUtf8(request.libraryRoot.lexically_normal())};

    if (request.scanAfterOpen)
    {
      arguments.emplace_back(kScanAfterOpenOption);
    }

    return arguments;
  }
} // namespace ao::desktop
