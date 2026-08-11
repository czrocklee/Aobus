// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/GtkStartupPlan.h"

#include <ao/Error.h>
#include <ao/rt/Log.h>
#include <ao/utility/Path.h>

#include <CLI/CLI.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    struct MutableArguments final
    {
      std::vector<std::string> strings;
      std::vector<char*> pointers;
    };

    MutableArguments makeMutableArguments(std::span<std::string_view const> const arguments)
    {
      auto mutableArguments = MutableArguments{};
      mutableArguments.strings.reserve(arguments.size());
      mutableArguments.pointers.reserve(arguments.size() + 1);

      for (auto const argument : arguments)
      {
        mutableArguments.strings.emplace_back(argument);
      }

      for (auto& argument : mutableArguments.strings)
      {
        mutableArguments.pointers.push_back(argument.data());
      }

      mutableArguments.pointers.push_back(nullptr);
      return mutableArguments;
    }

    Result<std::filesystem::path> parseAbsoluteLibraryRoot(std::string_view const value)
    {
      if (value.empty())
      {
        return makeError(
          Error::Code::InvalidInput, std::format("GTK startup argument '{}' requires a path", kLibraryRootOption));
      }

      try
      {
        auto root = utility::pathFromUtf8(value);

        if (!root.is_absolute())
        {
          return makeError(Error::Code::InvalidInput,
                           std::format("GTK startup argument '{}' requires an absolute path", kLibraryRootOption));
        }

        return root.lexically_normal();
      }
      catch (std::filesystem::filesystem_error const& error)
      {
        return makeError(
          Error::Code::InvalidInput, std::format("Invalid UTF-8 library root '{}': {}", value, error.what()));
      }
    }
  } // namespace

  Result<GtkStartupPlan> planGtkStartup(std::span<std::string_view const> const arguments)
  {
    if (arguments.empty() || arguments.front().empty())
    {
      return makeError(Error::Code::InvalidInput, "GTK startup arguments must include argv[0]");
    }

    auto plan = GtkStartupPlan{};
    auto cliApp = CLI::App{"Aobus Music Library"};
    cliApp.allow_extras();

    auto const logMapping = std::map<std::string, rt::LogLevel>{{"trace", rt::LogLevel::Trace},
                                                                {"debug", rt::LogLevel::Debug},
                                                                {"info", rt::LogLevel::Info},
                                                                {"warn", rt::LogLevel::Warn},
                                                                {"error", rt::LogLevel::Error},
                                                                {"critical", rt::LogLevel::Critical},
                                                                {"off", rt::LogLevel::Off}};
    std::int32_t verbosity = 0;
    bool successorRequested = false;
    bool scanAfterOpen = false;
    auto libraryRootValue = std::string{};
    auto mutableArguments = makeMutableArguments(arguments);

    auto* const verbosityOption = cliApp.add_flag("-v", verbosity, "Verbosity level (-v for debug, -vv for trace)");
    cliApp.add_option("--log-level", plan.logLevel, "Set the logging level")
      ->transform(CLI::CheckedTransformer{logMapping, CLI::ignore_case});
    cliApp.add_flag("--version", plan.showVersion, "Show version information");
    auto* const successorOption =
      cliApp.add_flag(std::string{kSuccessorOption}, successorRequested, "Internal successor startup")->group("");
    auto* const libraryRootOption =
      cliApp.add_option(std::string{kLibraryRootOption}, libraryRootValue, "Internal successor library root")
        ->take_last()
        ->group("");
    auto* const scanAfterOpenOption =
      cliApp.add_flag(std::string{kScanAfterOpenOption}, scanAfterOpen, "Internal successor bootstrap scan")->group("");

    try
    {
      cliApp.parse(static_cast<std::int32_t>(arguments.size()), mutableArguments.pointers.data());
    }
    catch (CLI::ParseError const& error)
    {
      plan.exitCode = cliApp.exit(error);
      plan.shouldExit = true;
      return plan;
    }

    // CLI11 stores unclaimed arguments in their original order. Its
    // remaining_for_passthrough() helper reverses them for another CLI11
    // parser, while GTK expects a conventional argv in process order.
    plan.gtkArguments = cliApp.remaining();
    plan.gtkArguments.insert(plan.gtkArguments.begin(), std::string{arguments.front()});

    if (plan.showVersion)
    {
      plan.shouldExit = true;
      return plan;
    }

    if (verbosityOption->count() > 0)
    {
      plan.logLevel = verbosity == 1 ? rt::LogLevel::Debug : rt::LogLevel::Trace;
    }

    if (successorOption->count() > 1)
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("GTK startup argument '{}' may only be specified once", kSuccessorOption));
    }

    if (libraryRootOption->count() > 1)
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("GTK startup argument '{}' may only be specified once", kLibraryRootOption));
    }

    if (scanAfterOpenOption->count() > 1)
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("GTK startup argument '{}' may only be specified once", kScanAfterOpenOption));
    }

    auto const hasSuccessorOption = successorOption->count() == 1;
    auto const hasLibraryRootOption = libraryRootOption->count() == 1;
    auto const hasScanAfterOpenOption = scanAfterOpenOption->count() == 1;

    if (hasSuccessorOption != hasLibraryRootOption)
    {
      return makeError(
        Error::Code::InvalidInput,
        std::format(
          "GTK successor arguments '{}' and '{}' must be specified together", kSuccessorOption, kLibraryRootOption));
    }

    if (hasScanAfterOpenOption && !hasSuccessorOption)
    {
      return makeError(Error::Code::InvalidInput,
                       std::format("GTK startup argument '{}' requires a successor startup", kScanAfterOpenOption));
    }

    if (hasSuccessorOption)
    {
      auto rootRes = parseAbsoluteLibraryRoot(libraryRootValue);

      if (!rootRes)
      {
        return std::unexpected{rootRes.error()};
      }

      plan.registrationMode = GtkApplicationRegistrationMode::ReplaceExisting;
      plan.optSuccessorLibraryRoot = std::move(*rootRes);
      plan.scanAfterOpen = hasScanAfterOpenOption;
    }

    return plan;
  }

  std::optional<std::string> incompleteSuccessorStartupDiagnostic(GtkApplicationRegistrationMode const registrationMode,
                                                                  bool const startupCompleted,
                                                                  std::int32_t const exitCode)
  {
    if (registrationMode != GtkApplicationRegistrationMode::ReplaceExisting || startupCompleted)
    {
      return std::nullopt;
    }

    if (exitCode == 0)
    {
      return "Aobus successor exited before application activation completed";
    }

    return std::format("Aobus successor application registration failed with exit code {}", exitCode);
  }
} // namespace ao::gtk
