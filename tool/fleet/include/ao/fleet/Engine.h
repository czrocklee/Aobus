// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ao::fleet
{
  struct SearchRequest
  {
    std::string objectiveId;
    std::size_t budget = 0;
  };

  struct ObjectiveDefinition
  {
    std::string id;
    std::string property;
  };

  struct FrontierResult
  {
    std::string candidateId;
    double score = 0.0;
  };

  struct EngineContext
  {
    std::filesystem::path realRepo;
    std::filesystem::path immutableBase;
    std::filesystem::path runRoot;
    Registry const& registry;
    IProcessRunner& processRunner;
    async::Runtime& asyncRuntime;
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

  class Scheduler final
  {
  public:
    static Result<std::vector<std::string>> order(std::vector<PhaseIntent> const& intents);
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
