// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ao::fleet
{
  struct EngineContext
  {
    std::filesystem::path realRepo;
    std::filesystem::path immutableBase;
    std::filesystem::path runRoot;
    Registry const& registry;
    IProcessRunner& processRunner;
    async::Runtime& asyncRuntime;
    // Oracle id -> version fingerprint, resolved once per run from the immutable base.
    std::map<std::string, std::string, std::less<>> oracleVersions;
  };

  class IEngine
  {
  public:
    IEngine() = default;
    virtual ~IEngine() = default;

    IEngine(IEngine const&) = delete;
    IEngine& operator=(IEngine const&) = delete;
    IEngine(IEngine&&) = delete;
    IEngine& operator=(IEngine&&) = delete;

    virtual Result<ReviewManifest> execute(ResolvedPhase const& phase, EngineContext const& context) = 0;
  };

  class GateEngine final : public IEngine
  {
  public:
    Result<ReviewManifest> execute(ResolvedPhase const& phase, EngineContext const& context) override;
  };

  class SynthesisEngine final : public IEngine
  {
  public:
    Result<ReviewManifest> execute(ResolvedPhase const& phase, EngineContext const& context) override;
  };

  // Static validation of the intent dependency graph (dangling references, cycles).
  // Execution order is decided at runtime by the completion-driven scheduler, which
  // dispatches whichever phases become ready as their dependencies finish.
  class Scheduler final
  {
  public:
    static Result<> validate(std::vector<PhaseIntent> const& intents);
  };

  struct RunSummary
  {
    std::vector<ReviewManifest> manifests;
    bool escalated = false;
  };

  class FleetRunner final
  {
  public:
    explicit FleetRunner(IProcessRunner& processRunner);

    Result<RunSummary> run(Registry const& registry,
                           std::vector<PhaseIntent> const& intents,
                           std::filesystem::path const& repo,
                           std::filesystem::path const& out);

  private:
    IProcessRunner& _processRunner;
  };
} // namespace ao::fleet
