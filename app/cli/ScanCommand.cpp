// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "Output.h"
#include "ScanOutput.h"
#include <ao/library/LibraryScanner.h>
#include <ao/library/ScanPlanExecutor.h>

#include <CLI/App.hpp>

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <print>
#include <string>
#include <string_view>
#include <utility>

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

    void printScanItemYaml(library::ScanItem const& item, std::ostream& os)
    {
      std::println(os, "  - type: {}", yamlQuote(scanClassificationName(item.classification)));
      yamlKeyValue(os, 4, "uri", itemLabel(item));

      if (!item.errorMessage.empty())
      {
        yamlKeyValue(os, 4, "message", item.errorMessage);
      }
    }

    void printScanItemJson(library::ScanItem const& item, std::ostream& os)
    {
      auto object = JsonObject{os};
      object.stringField("type", scanClassificationName(item.classification));
      object.stringField("uri", itemLabel(item));

      if (!item.errorMessage.empty())
      {
        object.stringField("message", item.errorMessage);
      }
    }

    void printScanResult(library::ScanPlan const& plan, bool dryRun, OutputFormat format, std::ostream& os)
    {
      auto const newCount = plan.count(library::ScanClassification::New);
      auto const changedCount = plan.count(library::ScanClassification::Changed);
      auto const missingCount = plan.count(library::ScanClassification::Missing);
      auto const unchangedCount = plan.count(library::ScanClassification::Unchanged);
      auto const errorCount = plan.count(library::ScanClassification::Error);

      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "new", static_cast<std::uint64_t>(newCount));
        yamlKeyValue(os, 0, "changed", static_cast<std::uint64_t>(changedCount));
        yamlKeyValue(os, 0, "missing", static_cast<std::uint64_t>(missingCount));
        yamlKeyValue(os, 0, "unchanged", static_cast<std::uint64_t>(unchangedCount));
        yamlKeyValue(os, 0, "errors", static_cast<std::uint64_t>(errorCount));

        if (!dryRun)
        {
          return;
        }

        bool hasItems = false;

        for (auto const& item : plan.items)
        {
          if (item.classification == library::ScanClassification::Unchanged)
          {
            continue;
          }

          if (!hasItems)
          {
            std::println(os, "items:");
            hasItems = true;
          }

          printScanItemYaml(item, os);
        }

        if (!hasItems)
        {
          std::println(os, "items: []");
        }

        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.uintField("new", static_cast<std::uint64_t>(newCount));
        object.uintField("changed", static_cast<std::uint64_t>(changedCount));
        object.uintField("missing", static_cast<std::uint64_t>(missingCount));
        object.uintField("unchanged", static_cast<std::uint64_t>(unchangedCount));
        object.uintField("errors", static_cast<std::uint64_t>(errorCount));

        if (dryRun)
        {
          object.field("items");
          auto array = JsonArray{os};

          for (auto const& item : plan.items)
          {
            if (item.classification == library::ScanClassification::Unchanged)
            {
              continue;
            }

            array.element();
            printScanItemJson(item, os);
          }
        }

        object.close();
        std::println(os);
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
