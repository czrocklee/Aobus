// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "Output.h"
#include "ScanOutput.h"
#include <ao/library/LibraryScanner.h>
#include <ao/library/ScanPlanExecutor.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::cli
{
  struct ScanItemDto final
  {
    std::string type{};
    std::string uri{};
    std::optional<std::string> optMessage{};
  };

  struct ScanReportDto final
  {
    bool dryRun = false;
    std::uint64_t newCount = 0;
    std::uint64_t changed = 0;
    std::uint64_t missing = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t errors = 0;
    std::optional<std::vector<ScanItemDto>> optItems{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ScanItemDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optMessage")
    {
      return "message";
    }

    return memberName;
  }
};

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::ScanReportDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "newCount")
    {
      return "new";
    }

    if (memberName == "optItems")
    {
      return "items";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    std::string itemLabel(library::ScanItem const& item)
    {
      if (!item.uri.empty())
      {
        return item.uri;
      }

      return item.fullPath.generic_string();
    }

    ScanItemDto scanItemDto(library::ScanItem const& item)
    {
      return ScanItemDto{.type = std::string{scanClassificationName(item.classification)},
                         .uri = itemLabel(item),
                         .optMessage = item.errorMessage.empty() ? std::nullopt : std::optional{item.errorMessage}};
    }

    void printScanResult(library::ScanPlan const& plan, bool dryRun, OutputFormat format, std::ostream& os)
    {
      auto const newCount = plan.count(library::ScanClassification::New);
      auto const changedCount = plan.count(library::ScanClassification::Changed);
      auto const missingCount = plan.count(library::ScanClassification::Missing);
      auto const unchangedCount = plan.count(library::ScanClassification::Unchanged);
      auto const errorCount = plan.count(library::ScanClassification::Error);

      if (format != OutputFormat::Plain)
      {
        auto report = ScanReportDto{.dryRun = dryRun,
                                    .newCount = static_cast<std::uint64_t>(newCount),
                                    .changed = static_cast<std::uint64_t>(changedCount),
                                    .missing = static_cast<std::uint64_t>(missingCount),
                                    .unchanged = static_cast<std::uint64_t>(unchangedCount),
                                    .errors = static_cast<std::uint64_t>(errorCount)};

        if (dryRun)
        {
          report.optItems = std::vector<ScanItemDto>{};

          for (auto const& item : plan.items)
          {
            if (item.classification == library::ScanClassification::Unchanged)
            {
              continue;
            }

            report.optItems->push_back(scanItemDto(item));
          }
        }

        emitDocument(os, format, report);
        return;
      }

      std::println(os,
                   "new {}  changed {}  missing {}  unchanged {}  errors {}",
                   newCount,
                   changedCount,
                   missingCount,
                   unchangedCount,
                   errorCount);

      if (!dryRun)
      {
        return;
      }

      for (auto const& item : plan.items)
      {
        if (item.classification == library::ScanClassification::Unchanged)
        {
          continue;
        }

        std::print(os, "{} {}", scanClassificationName(item.classification), itemLabel(item));

        if (!item.errorMessage.empty())
        {
          std::print(os, " {}", item.errorMessage);
        }

        std::println(os);
      }
    }

    void printFailure(library::ScanFailure const& failure, std::ostream& err)
    {
      if (failure.uri.empty())
      {
        std::println(err, "failed to {}: {}", failure.stage, failure.message);
        return;
      }

      std::println(err, "failed to {} {}: {}", failure.stage, failure.uri, failure.message);
    }
  } // namespace

  void runScan(CliContext& context, bool dryRun, bool verbose)
  {
    auto& ml = context.musicLibrary();
    auto scanner = library::LibraryScanner{ml};
    auto planResult = scanner.buildPlan(
      verbose
        ? library::LibraryScanner::ProgressCallback{[&context](std::filesystem::path const& path)
                                                    {
                                                      std::println(context.io().err, "scan: {}", path.generic_string());
                                                    }}
        : nullptr);

    if (!planResult)
    {
      auto const& error = planResult.error();
      throwCommandError(error, "scan failed: {}", error.message);
    }

    auto plan = std::move(*planResult);
    printScanResult(plan, dryRun, context.options().format, context.io().out);

    if (dryRun)
    {
      return;
    }

    auto executor = library::ScanPlanExecutor{
      ml,
      std::move(plan),
      verbose ? library::ScanPlanExecutor::ProgressCallback{[&context](std::filesystem::path const& path, std::int32_t)
                                                            {
                                                              if (!path.empty())
                                                              {
                                                                std::println(
                                                                  context.io().err, "apply: {}", path.generic_string());
                                                              }
                                                            }}
              : nullptr,
      [&context](library::ScanFailure const& failure) { printFailure(failure, context.io().err); }};

    if (auto const applyResult = executor.run(); !applyResult)
    {
      auto const& error = applyResult.error();
      throwCommandError(error, "scan apply failed: {}", error.message);
    }
  }

  void setupScanCommand(CLI::App& app, CliContext& context)
  {
    auto* const cmd = app.add_subcommand("scan", "Scan music root and reconcile the library");
    auto* const dryRun = cmd->add_flag("--dry-run", "show planned changes without mutating the library");
    auto* const verbose = cmd->add_flag("--verbose", "print scan progress to stderr");

    cmd->callback([&context, dryRun, verbose] { runScan(context, dryRun->count() > 0, verbose->count() > 0); });
  }
} // namespace ao::cli
