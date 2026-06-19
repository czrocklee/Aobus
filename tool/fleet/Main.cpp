// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Engine.h"
#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include "fleet/RouteStore.h"
#include "fleet/Serialization.h"
#include <ao/Error.h>

#include <CLI/CLI.hpp>

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace
{
  constexpr auto kPolicyExit = std::int32_t{2};
  constexpr auto kInfrastructureExit = std::int32_t{3};
  constexpr auto kConfigurationExit = std::int32_t{5};
  constexpr auto kCliExit = std::int32_t{64};
  constexpr auto kDefaultStatsWindow = std::size_t{20};

  std::int32_t handleValidate(std::filesystem::path const& registryPath)
  {
    if (auto registry = ao::fleet::loadRegistry(registryPath); !registry)
    {
      std::cerr << registry.error().message << '\n';
      return kConfigurationExit;
    }

    std::cout << "registry valid\n";
    return 0;
  }

  std::int32_t handleRun(std::filesystem::path const& registryPath,
                         std::vector<std::filesystem::path> const& intentPaths,
                         std::filesystem::path const& repo,
                         std::filesystem::path const& out)
  {
    auto registry = ao::fleet::loadRegistry(registryPath);

    if (!registry)
    {
      std::cerr << registry.error().message << '\n';
      return kConfigurationExit;
    }

    auto intents = ao::fleet::loadIntents(intentPaths);

    if (!intents)
    {
      std::cerr << intents.error().message << '\n';
      return kCliExit;
    }

    for (auto const& intent : *intents)
    {
      if (auto resolved = ao::fleet::resolvePhase(*registry, intent); !resolved)
      {
        std::cerr << resolved.error().message << '\n';
        return kCliExit;
      }
    }

    auto runner = ao::fleet::BoostProcessRunner{};
    auto result = ao::fleet::FleetRunner{runner}.run(*registry, *intents, repo, out);

    if (!result)
    {
      std::cerr << result.error().message << '\n';
      return result.error().code == ao::Error::Code::IoError ? kInfrastructureExit : kPolicyExit;
    }

    for (auto const& manifest : result->manifests)
    {
      std::cout << manifest.phaseId << ' ' << ao::fleet::toString(manifest.mode) << ' '
                << ao::fleet::toString(manifest.failure) << '\n';
    }

    return result->escalated ? kPolicyExit : 0;
  }

  std::int32_t handleReview(std::filesystem::path const& reviewOut,
                            std::string const& reviewPhase,
                            std::string const& verdictText,
                            std::string const& reviewReason)
  {
    auto optVerdict = ao::fleet::parseReviewVerdict(verdictText);

    if (!optVerdict)
    {
      std::cerr << "invalid verdict: " << verdictText << '\n';
      return kCliExit;
    }

    auto result = ao::fleet::RouteStore{reviewOut}.record(reviewPhase, *optVerdict, reviewReason);

    if (!result)
    {
      std::cerr << result.error().message << '\n';
      return kInfrastructureExit;
    }

    return 0;
  }

  std::int32_t handleStats(std::filesystem::path const& statsOut, std::size_t window)
  {
    bool trailingCorruption = false;
    auto result = ao::fleet::RouteStore{statsOut}.statistics(window, &trailingCorruption);

    if (!result)
    {
      std::cerr << result.error().message << '\n';
      return kInfrastructureExit;
    }

    if (trailingCorruption)
    {
      std::cerr << "warning: ignored incomplete trailing review-outcome document\n";
    }

    std::cout << "schema: aobus-fleet-route-stats/v1\nwindow-count: " << window << "\nroutes:\n";

    for (auto const& row : *result)
    {
      std::cout << "  - route-key: " << ao::fleet::yamlScalar(row.route) << "\n"
                << "    usable-count: " << row.usable << "\n"
                << "    unusable-count: " << row.unusable << "\n"
                << "    paused: " << (row.paused ? "true" : "false") << "\n";
    }

    return 0;
  }

  std::int32_t handleRouteReset(std::filesystem::path const& resetOut, std::string const& resetRoute)
  {
    auto result = ao::fleet::RouteStore{resetOut}.reset(resetRoute);

    if (!result)
    {
      std::cerr << result.error().message << '\n';
      return kInfrastructureExit;
    }

    return 0;
  }
} // namespace

int main(int argc, char** argv)
{
  // Writing to agent/oracle stdin pipes (or to our own stdout when piped to a pager) must
  // surface EPIPE as an error code instead of terminating the whole fleet run.
  std::signal(SIGPIPE, SIG_IGN);

  try
  {
    auto app = CLI::App{"Aobus delegated agent fleet"};
    app.require_subcommand(1);

    auto registryPath = std::filesystem::path{};
    auto* validate = app.add_subcommand("validate-config", "Validate the complete fleet registry");
    validate->add_option("--registry", registryPath)->required()->check(CLI::ExistingFile);

    auto runRegistry = std::filesystem::path{};
    auto repo = std::filesystem::path{};
    auto out = std::filesystem::path{};
    auto intentPaths = std::vector<std::filesystem::path>{};
    auto* run = app.add_subcommand("run", "Run one or more intent files");
    run->add_option("--registry", runRegistry)->required()->check(CLI::ExistingFile);
    run->add_option("--repo", repo)->required()->check(CLI::ExistingDirectory);
    run->add_option("--out", out)->required();
    run->add_option("intent", intentPaths)->required()->check(CLI::ExistingFile);

    auto reviewOut = std::filesystem::path{};
    auto reviewPhase = std::string{};
    auto verdictText = std::string{};
    auto reviewReason = std::string{};
    auto* review = app.add_subcommand("review", "Manage chair review outcomes");
    auto* record = review->add_subcommand("record", "Record a terminal chair verdict");
    record->add_option("--out", reviewOut)->required();
    record->add_option("--phase", reviewPhase)->required();
    record->add_option("--verdict", verdictText)->required();
    record->add_option("--reason", reviewReason)->required();

    auto statsOut = std::filesystem::path{};
    auto window = kDefaultStatsWindow;
    auto* stats = app.add_subcommand("stats", "Print route competence statistics as YAML");
    stats->add_option("--out", statsOut)->required();
    stats->add_option("--window", window)->default_val(kDefaultStatsWindow);

    auto resetOut = std::filesystem::path{};
    auto resetRoute = std::string{};
    auto* route = app.add_subcommand("route", "Manage route breaker state");
    auto* reset = route->add_subcommand("reset", "Reset one route and append an audit event");
    reset->add_option("--out", resetOut)->required();
    reset->add_option("--route", resetRoute)->required();

    try
    {
      app.parse(argc, argv);
    }
    catch (CLI::ParseError const& error)
    {
      return app.exit(error);
    }

    if (*validate)
    {
      return handleValidate(registryPath);
    }

    if (*run)
    {
      return handleRun(runRegistry, intentPaths, repo, out);
    }

    if (*record)
    {
      return handleReview(reviewOut, reviewPhase, verdictText, reviewReason);
    }

    if (*stats)
    {
      return handleStats(statsOut, window);
    }

    if (*reset)
    {
      return handleRouteReset(resetOut, resetRoute);
    }
  }
  catch (std::exception const& exception)
  {
    std::cerr << "fatal error: " << exception.what() << '\n';
    return kInfrastructureExit;
  }
  catch (...)
  {
    std::cerr << "unknown fatal error\n";
    return kInfrastructureExit;
  }

  return kCliExit;
}
