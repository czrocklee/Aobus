// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ScanCommand.h"

#include "CliRuntime.h"
#include "CommandError.h"
#include "Output.h"
#include "ScanOutput.h"
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
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
    std::uint64_t moved = 0;
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
    std::string itemLabel(rt::ScanItem const& item)
    {
      if (!item.uri.empty())
      {
        return item.uri;
      }

      return item.fullPath.generic_string();
    }

    ScanItemDto toScanItemDto(rt::ScanItem const& item)
    {
      return ScanItemDto{.type = std::string{scanClassificationName(item.classification)},
                         .uri = itemLabel(item),
                         .optMessage = item.errorMessage.empty() ? std::nullopt : std::optional{item.errorMessage}};
    }

    void printScanResult(rt::ScanPlan const& plan, bool dryRun, OutputFormat format, std::ostream& os)
    {
      auto const newCount = plan.count(rt::ScanClassification::New);
      auto const changedCount = plan.count(rt::ScanClassification::Changed);
      auto const movedCount = plan.count(rt::ScanClassification::Moved);
      auto const missingCount = plan.count(rt::ScanClassification::Missing);
      auto const unchangedCount = plan.count(rt::ScanClassification::Unchanged);
      auto const errorCount = plan.count(rt::ScanClassification::Error);

      if (format != OutputFormat::Plain)
      {
        auto report = ScanReportDto{.dryRun = dryRun,
                                    .newCount = static_cast<std::uint64_t>(newCount),
                                    .changed = static_cast<std::uint64_t>(changedCount),
                                    .moved = static_cast<std::uint64_t>(movedCount),
                                    .missing = static_cast<std::uint64_t>(missingCount),
                                    .unchanged = static_cast<std::uint64_t>(unchangedCount),
                                    .errors = static_cast<std::uint64_t>(errorCount)};

        if (dryRun)
        {
          report.optItems = std::vector<ScanItemDto>{};

          for (auto const& item : plan.items)
          {
            if (item.classification == rt::ScanClassification::Unchanged)
            {
              continue;
            }

            report.optItems->push_back(toScanItemDto(item));
          }
        }

        emitDocument(os, format, report);
        return;
      }

      std::println(os,
                   "new {}  changed {}  moved {}  missing {}  unchanged {}  errors {}",
                   newCount,
                   changedCount,
                   movedCount,
                   missingCount,
                   unchangedCount,
                   errorCount);

      if (!dryRun)
      {
        return;
      }

      for (auto const& item : plan.items)
      {
        if (item.classification == rt::ScanClassification::Unchanged)
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

    void printFailure(rt::ScanFailure const& failure, std::ostream& err)
    {
      if (failure.uri.empty())
      {
        std::println(err, "failed to {}: {}", failure.stage, failure.message);
        return;
      }

      std::println(err, "failed to {} {}: {}", failure.stage, failure.uri, failure.message);
    }

    void printApplySummary(rt::ScanApplyResult const& result, std::ostream& os)
    {
      if (result.relinkedCount > 0)
      {
        std::println(os, "Relinked {} moved file{}", result.relinkedCount, result.relinkedCount == 1 ? "" : "s");
      }

      if (result.missingCount > 0)
      {
        std::println(os,
                     "{} missing file{} need{} review",
                     result.missingCount,
                     result.missingCount == 1 ? "" : "s",
                     result.missingCount == 1 ? "s" : "");
      }
    }

    std::string_view scanApplyProgressLabel(rt::ScanApplyProgressStage stage)
    {
      switch (stage)
      {
        case rt::ScanApplyProgressStage::Updating: return "apply";
        case rt::ScanApplyProgressStage::Fingerprinting: return "fingerprint";
      }

      return "apply";
    }
  } // namespace

  void runScan(CliRuntime& cli, bool dryRun, bool verbose, bool deferFingerprint)
  {
    auto& ml = cli.musicLibrary();
    auto scanService = rt::LibraryScan{ml};
    auto buildProgress = rt::LibraryScan::BuildProgressCallback{};

    if (verbose)
    {
      buildProgress = [&cli](std::filesystem::path const& path)
      { std::println(cli.io().err, "scan: {}", path.generic_string()); };
    }

    auto planResult = scanService.buildPlan(std::move(buildProgress));

    if (!planResult)
    {
      auto const& error = planResult.error();
      throwCommandError(error, "scan failed: {}", error.message);
    }

    auto plan = std::move(*planResult);
    printScanResult(plan, dryRun, cli.options().format, cli.io().out);

    if (dryRun)
    {
      return;
    }

    auto options = rt::ScanApplyOptions{};

    if (deferFingerprint)
    {
      options.audioIdentityPolicy = rt::AudioIdentityPolicy::DeferNew;
    }

    auto applyProgress = rt::LibraryScan::ApplyProgressCallback{};

    if (verbose)
    {
      applyProgress = [&cli](rt::ScanApplyProgress const& progress)
      {
        if (!progress.path.empty())
        {
          std::println(cli.io().err, "{}: {}", scanApplyProgressLabel(progress.stage), progress.path.generic_string());
        }
      };
    }

    if (auto const applyResult =
          scanService.applyPlan(std::move(plan),
                                options,
                                std::move(applyProgress),
                                [&cli](rt::ScanFailure const& failure) { printFailure(failure, cli.io().err); });
        !applyResult)
    {
      auto const& error = applyResult.error();
      throwCommandError(error, "scan apply failed: {}", error.message);
    }
    else if (cli.options().format == OutputFormat::Plain)
    {
      printApplySummary(*applyResult, cli.io().out);

      if (deferFingerprint && !applyResult->cancelled)
      {
        std::println(
          cli.io().out,
          "Audio identity fingerprinting was deferred; run `aobus lib fingerprint --pending` to finish indexing.");
      }
    }
  }

  void configureScanCommand(CLI::App& app, CliRuntime& cli)
  {
    auto* const cmd = app.add_subcommand("scan", "Scan music root and reconcile the library");
    auto* const dryRun = cmd->add_flag("--dry-run", "show planned changes without mutating the library");
    auto* const verbose = cmd->add_flag("--verbose", "print scan progress to stderr");
    auto* const deferFingerprint = cmd->add_flag("--defer-fingerprint", "defer new-file audio fingerprinting");

    cmd->callback([&cli, dryRun, verbose, deferFingerprint]
                  { runScan(cli, dryRun->count() > 0, verbose->count() > 0, deferFingerprint->count() > 0); });
  }
} // namespace ao::cli
