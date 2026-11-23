// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DesktopApplication.h"
#include <ao/Error.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/utility/Path.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::appkit
{
  namespace
  {
    constexpr std::string_view kStateRootOption = "--state-root";
    constexpr std::string_view kLibraryOption = "--library";
    constexpr std::string_view kScenarioOption = "--scenario";

    bool isPathOption(std::string_view argument)
    {
      return argument == kStateRootOption || argument == kLibraryOption;
    }

    template<typename Arguments>
    bool hasOptionValueAt(Arguments const& arguments, std::size_t index)
    {
      if (index >= arguments.size())
      {
        return false;
      }

      auto const value = std::string_view{arguments[index]};
      return !value.empty() && !value.starts_with('-');
    }

    std::unexpected<Error> missingPathOperand(std::string_view option)
    {
      return makeError(Error::Code::InvalidInput, std::format("Startup argument '{}' requires a path", option));
    }

    Result<std::filesystem::path> resolvePathArgument(std::string_view option, std::string_view value)
    {
      auto path = std::filesystem::path{};

      try
      {
        path = utility::pathFromUtf8(value);
      }
      catch (std::filesystem::filesystem_error const& error)
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Invalid path for startup argument '{}': {}", option, error.what()));
      }

      auto error = std::error_code{};
      auto absolute = std::filesystem::absolute(path, error);

      if (error)
      {
        return makeError(
          Error::Code::IoError,
          std::format("Cannot resolve path for startup argument '{}' ('{}'): {}", option, value, error.message()));
      }

      return absolute;
    }

    Result<> validateRawPathOperands(std::span<std::string_view const> arguments, bool allowScenario)
    {
      // Check adjacency in the original argv, before any successor options were removed.
      for (std::size_t index = 0; index < arguments.size(); ++index)
      {
        auto const option = arguments[index];

        // Let ordered parsing diagnose a bad scenario slot before interpreting its next token as a path option.
        if (allowScenario && option == kScenarioOption && !hasOptionValueAt(arguments, index + 1))
        {
          break;
        }

        if (!isPathOption(option))
        {
          continue;
        }

        ++index;

        if (!hasOptionValueAt(arguments, index))
        {
          return missingPathOperand(option);
        }
      }

      return {};
    }
  } // namespace

  Result<DesktopStartupArguments> parseDesktopStartupArguments(std::span<std::string_view const> arguments,
                                                               bool allowScenario)
  {
    auto protocolRes = desktop::parseLibrarySuccessorProtocol(arguments);
    auto const acceptsScenario = allowScenario && protocolRes && !protocolRes->optRequest;

    if (auto validationRes = validateRawPathOperands(arguments, acceptsScenario); !validationRes)
    {
      return std::unexpected{validationRes.error()};
    }

    if (!protocolRes)
    {
      return std::unexpected{protocolRes.error()};
    }

    auto startup = DesktopStartupArguments{};
    startup.successor = protocolRes->optRequest.has_value();
    startup.launch.optRequest = std::move(protocolRes->optRequest);
    auto const& remaining = protocolRes->remainingArguments;

    for (std::size_t index = 0; index < remaining.size(); ++index)
    {
      if (auto const option = std::string_view{remaining[index]}; isPathOption(option))
      {
        ++index;

        if (!hasOptionValueAt(remaining, index))
        {
          return missingPathOperand(option);
        }

        if (option == kLibraryOption && startup.launch.optRequest)
        {
          return makeError(Error::Code::InvalidInput,
                           startup.successor
                             ? "Startup argument '--library' cannot be combined with a successor request"
                             : "Startup argument '--library' may only be specified once");
        }

        auto pathRes = resolvePathArgument(option, remaining[index]);

        if (!pathRes)
        {
          return std::unexpected{pathRes.error()};
        }

        if (option == kStateRootOption)
        {
          startup.launch.stateRoot = std::move(*pathRes);
        }
        else
        {
          // Opening the selected root does not request a rescan.
          startup.launch.optRequest = desktop::LibrarySwitchRequest{.libraryRoot = std::move(*pathRes)};
        }
      }
      else if (acceptsScenario && option == kScenarioOption)
      {
        ++index;

        if (!hasOptionValueAt(remaining, index))
        {
          return makeError(Error::Code::InvalidInput, "Startup argument '--scenario' requires a value");
        }

        startup.scenario = remaining[index];
      }
      else
      {
        return makeError(
          Error::Code::InvalidInput, "Invalid arguments: expected --state-root <path> or --library <path>.");
      }
    }

    return startup;
  }
} // namespace ao::appkit
