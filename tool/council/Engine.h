// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "council/CouncilSchema.h"
#include "council/ProcessRunner.h"
#include <ao/Error.h>

#include <filesystem>
#include <vector>

namespace ao::async
{
  class Runtime;
}

namespace ao::council
{
  struct EngineContext
  {
    std::filesystem::path realRepo;
    std::filesystem::path immutableBase;
    std::filesystem::path runRoot;
    Registry const& registry;
    ProcessRunner& processRunner;
    async::Runtime& asyncRuntime;
  };

  class Engine final
  {
  public:
    Result<ReviewManifest> execute(ResolvedPhase const& phase, EngineContext const& context);
  };

  // Static validation of the intent dependency graph (dangling references, cycles).
  // Runtime scheduling still waits for dependency outcomes before launching a phase.
  class Scheduler final
  {
  public:
    static Result<> validate(std::vector<PhaseIntent> const& intents);
  };

  struct RunSummary
  {
    std::vector<ReviewManifest> manifests = {};
    bool failed = false;
  };

  class Runner final
  {
  public:
    explicit Runner(ProcessRunner& processRunner);

    Result<RunSummary> run(Registry const& registry,
                           std::vector<PhaseIntent> const& intents,
                           std::filesystem::path const& repo,
                           std::filesystem::path const& out);

  private:
    ProcessRunner& _processRunner;
  };
} // namespace ao::council
