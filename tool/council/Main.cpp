// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Engine.h"
#include "council/Model.h"
#include "council/ProcessRunner.h"
#include "council/Serialization.h"
#include <ao/Error.h>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

namespace
{
  constexpr auto kPolicyExit = std::int32_t{2};
  constexpr auto kInfrastructureExit = std::int32_t{3};
  constexpr auto kConfigurationExit = std::int32_t{5};
  constexpr auto kCliExit = std::int32_t{64};

  bool hasFailure(ao::council::RunSummary const& summary, ao::council::FailureReason failure)
  {
    return std::ranges::any_of(summary.manifests,
                               [failure](ao::council::ReviewManifest const& manifest)
                               { return manifest.failure == failure; });
  }

  std::int32_t handleValidate(std::filesystem::path const& registryPath)
  {
    if (auto registry = ao::council::loadRegistry(registryPath); !registry)
    {
      std::println(stderr, "{}", registry.error().message);
      return kConfigurationExit;
    }

    std::println("registry valid");
    return 0;
  }

  std::int32_t handleRun(std::filesystem::path const& registryPath,
                         std::vector<std::filesystem::path> const& intentPaths,
                         std::filesystem::path const& repo,
                         std::filesystem::path const& out)
  {
    auto registry = ao::council::loadRegistry(registryPath);

    if (!registry)
    {
      std::println(stderr, "{}", registry.error().message);
      return kConfigurationExit;
    }

    auto intents = ao::council::loadIntents(intentPaths);

    if (!intents)
    {
      std::println(stderr, "{}", intents.error().message);
      return kCliExit;
    }

    auto runner = ao::council::BoostProcessRunner{};
    auto result = ao::council::Runner{runner}.run(*registry, *intents, repo, out);

    if (!result)
    {
      std::println(stderr, "{}", result.error().message);

      if (result.error().code == ao::Error::Code::IoError)
      {
        return kInfrastructureExit;
      }

      if (result.error().code == ao::Error::Code::InvalidInput)
      {
        return kCliExit;
      }

      return kPolicyExit;
    }

    for (auto const& manifest : result->manifests)
    {
      std::println("{} {} {}", manifest.phaseId, ao::council::toString(manifest.failure), manifest.summary);
    }

    if (hasFailure(*result, ao::council::FailureReason::InfrastructureFailed))
    {
      return kInfrastructureExit;
    }

    return result->failed ? kPolicyExit : 0;
  }
} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
  // Agent stdin pipes and our own stdout may close early; report EPIPE as an error
  // instead of terminating the entire council process.
  std::signal(SIGPIPE, SIG_IGN);

  try
  {
    auto app = CLI::App{"Aobus council runner"};
    app.require_subcommand(1);

    auto registryPath = std::filesystem::path{};
    auto* validate = app.add_subcommand("validate-config", "Validate the council registry");
    validate->add_option("--registry", registryPath)->required()->check(CLI::ExistingFile);

    auto runRegistry = std::filesystem::path{};
    auto repo = std::filesystem::path{};
    auto out = std::filesystem::path{};
    auto intentPaths = std::vector<std::filesystem::path>{};
    auto* run = app.add_subcommand("run", "Run one or more council intent files");
    run->add_option("--registry", runRegistry)->required()->check(CLI::ExistingFile);
    run->add_option("--repo", repo)->required()->check(CLI::ExistingDirectory);
    run->add_option("--out", out)->required();
    run->add_option("intent", intentPaths)->required()->check(CLI::ExistingFile);

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
  }
  catch (std::exception const& exception)
  {
    std::println(stderr, "fatal error: {}", exception.what());
    return kInfrastructureExit;
  }
  catch (...)
  {
    std::println(stderr, "unknown fatal error");
    return kInfrastructureExit;
  }

  return kCliExit;
}
