// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/fleet/Engine.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>
#include <ao/fleet/RouteStore.h>
#include <ao/fleet/Serialization.h>
#include <ao/fleet/Substrate.h>

#include <boost/interprocess/sync/file_lock.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    struct Candidate final
    {
      PatchArtifact patch;
      ProcessResult worker;
      PatchGuardResult guard;
      AgentDefinition agent;
      AuthorityPolicy authority;
    };

    struct GateFallback final
    {
      std::optional<AgentDefinition> optAgent;
      std::size_t rounds = 0;
    };

    struct GateRoundConfig final
    {
      AgentDefinition agent;
      AuthorityPolicy authority;
    };

    constexpr auto kFnvOffsetBasis = std::uint64_t{1469598103934665603ULL};
    constexpr auto kFnvPrime = std::uint64_t{1099511628211ULL};

    std::string replaceAll(std::string value, std::string_view from, std::string_view to)
    {
      auto cursor = std::size_t{};

      while ((cursor = value.find(from, cursor)) != std::string::npos)
      {
        value.replace(cursor, from.size(), to);
        cursor += to.size();
      }

      return value;
    }

    ProcessRequest agentRequest(AgentDefinition const& agent,
                                PhaseIntent const& intent,
                                std::filesystem::path const& workspace,
                                std::filesystem::path const& realRepo,
                                std::string prompt)
    {
      auto const promptFile = workspace / ".git" / "aobus-fleet-prompt.md";
      auto const intentFile = workspace / ".git" / "aobus-fleet-intent.yaml";
      std::filesystem::create_directories(promptFile.parent_path());
      {
        auto output = std::ofstream{promptFile, std::ios::binary | std::ios::trunc};
        output << prompt;
      }
      {
        auto output = std::ofstream{intentFile, std::ios::binary | std::ios::trunc};
        output << emitIntent(intent);
      }

      auto request = ProcessRequest{};
      request.cwd = workspace;
      request.timeout = agent.timeout;
      request.environmentWhitelist = agent.environmentWhitelist;

      for (auto argument : agent.argvTemplate)
      {
        argument = replaceAll(std::move(argument), "{workspace}", realRepo.string());
        argument = replaceAll(std::move(argument), "{repo}", realRepo.string());
        argument =
          replaceAll(std::move(argument), "{intent}", (realRepo / ".git" / "aobus-fleet-intent.yaml").string());
        argument =
          replaceAll(std::move(argument), "{prompt-file}", (realRepo / ".git" / "aobus-fleet-prompt.md").string());
        argument = replaceAll(std::move(argument), "{prompt}", prompt);
        request.argv.push_back(std::move(argument));
      }

      if (agent.promptDelivery == PromptDelivery::Stdin)
      {
        request.standardInput = std::move(prompt);
      }

      return request;
    }

    std::string gatePrompt(ResolvedPhase const& phase, std::size_t round, std::string_view feedback)
    {
      auto out = std::ostringstream{};
      out << "You are a delegated implementation worker. Work only in the provided repository copy.\n";
      out << "Invariant: " << phase.intent.invariant << "\n";
      out << "Task:\n" << phase.intent.body << "\n";
      out << "Allowed paths and operations:\n";

      for (auto const& rule : phase.intent.scope)
      {
        out << "- " << rule.path.generic_string() << ":";

        for (auto operation : rule.operations)
        {
          out << ' ' << toString(operation);
        }

        out << '\n';
      }

      out << "Do not edit the fleet, build definitions, scripts, skills, configuration, or design documentation unless "
             "explicitly listed.\n";
      out << "Do not write to the real repository. Do not create commits. The harness extracts the patch.\n";
      out << "Round: " << round << "\n";

      if (!feedback.empty())
      {
        out << "Previous independent validation feedback:\n" << feedback << '\n';
      }

      return out.str();
    }

    std::vector<std::filesystem::path> rulerPaths(ResolvedPhase const& phase)
    {
      auto result = std::vector<std::filesystem::path>{
        "CMakeLists.txt", "cmake", "script", ".agents", "doc/design", "config/agent-fleet.yaml", "tool/fleet"};

      if (phase.optOracle)
      {
        result.insert(result.end(), phase.optOracle->rulerPaths.begin(), phase.optOracle->rulerPaths.end());
      }

      if (phase.optRiskOracle)
      {
        result.insert(result.end(), phase.optRiskOracle->rulerPaths.begin(), phase.optRiskOracle->rulerPaths.end());
      }

      return result;
    }

    std::string oracleVersion(OracleDefinition const& oracle, std::filesystem::path const& base)
    {
      auto hash = std::uint64_t{kFnvOffsetBasis};
      auto mix = [&hash](std::string_view value)
      {
        for (auto character : value)
        {
          hash ^= static_cast<unsigned char>(character);
          hash *= kFnvPrime;
        }
      };
      mix("aobus-fleet/v1");
      mix(oracle.id);
      mix(toString(oracle.runner));
      mix(oracle.property);

      for (auto const& [name, value] : oracle.arguments)
      {
        mix(name);
        mix(value);
      }

      for (auto const& path : oracle.rulerPaths)
      {
        mix(path.generic_string());

        if (auto const absolute = base / path; std::filesystem::is_regular_file(absolute))
        {
          auto input = std::ifstream{absolute, std::ios::binary};
          auto buffer = std::array<char, 8192>{};

          while (input)
          {
            input.read(buffer.data(), buffer.size());
            mix(std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
          }
        }
        else if (std::filesystem::is_directory(absolute))
        {
          if (auto fingerprint = TreeCanary::fingerprint(absolute); fingerprint)
          {
            mix(fingerprint->value);
          }
        }
      }

      return std::format("{:016x}", hash);
    }

    std::vector<std::string> oracleArgv(OracleDefinition const& oracle)
    {
      switch (oracle.runner)
      {
        case OracleRunner::TestAll: return {"./build.sh", "debug"};
        case OracleRunner::TestCore:
        {
          auto result = std::vector<std::string>{"./script/run-tests.sh", "--core"};

          if (auto filter = oracle.arguments.find("filter"); filter != oracle.arguments.end())
          {
            result.push_back(filter->second);
          }

          return result;
        }
        case OracleRunner::TestGtk:
        {
          auto result = std::vector<std::string>{"./script/run-tests.sh", "--gtk"};

          if (auto filter = oracle.arguments.find("filter"); filter != oracle.arguments.end())
          {
            result.push_back(filter->second);
          }

          return result;
        }
        case OracleRunner::TidyClean:
        {
          auto result = std::vector<std::string>{"./script/run-clang-tidy.sh"};

          if (auto scope = oracle.arguments.find("scope"); scope != oracle.arguments.end())
          {
            result.push_back(scope->second);
          }

          return result;
        }
        case OracleRunner::BuildDebug: return {"./build.sh", "debug", "--target", "aobus-gtk"};
        case OracleRunner::TestDelta:
        case OracleRunner::PublicSignatureDelta: return {};
      }

      return {};
    }

    RiskEvidence runRiskOracle(OracleDefinition const& oracle, PatchArtifact const& patch)
    {
      auto result = RiskEvidence{.oracleId = oracle.id, .fired = false, .detail = {}};

      if (oracle.runner == OracleRunner::TestDelta)
      {
        result.fired =
          std::ranges::none_of(patch.touchedFiles,
                               [](auto const& path)
                               {
                                 auto const value = path.generic_string();
                                 return value.starts_with("test/") || value.find("Test.cpp") != std::string::npos;
                               });
        result.detail = result.fired ? "patch changes no registered test path" : "test path changed";
      }
      else if (oracle.runner == OracleRunner::PublicSignatureDelta)
      {
        result.fired =
          std::ranges::any_of(patch.touchedFiles,
                              [](auto const& path)
                              {
                                auto const value = path.generic_string();
                                return (value.starts_with("include/") || value.starts_with("app/include/")) &&
                                       (value.ends_with(".h") || value.ends_with(".hpp"));
                              });
        result.detail = result.fired ? "public header changed" : "no public header changed";
      }

      return result;
    }

    constexpr auto kDefaultOracleTimeout = std::chrono::minutes{30};

    Result<OracleEvidence> runOracle(OracleDefinition const& oracle,
                                     PatchArtifact const& patch,
                                     [[maybe_unused]] ResolvedPhase const& phase,
                                     EngineContext const& context,
                                     std::filesystem::path const& destination)
    {
      auto snapshot = SnapshotProvider{context.processRunner};
      auto extractor = PatchExtractor{context.processRunner};
      auto workspace = snapshot.createWorkspace(context.immutableBase, destination);

      if (!workspace)
      {
        return std::unexpected{workspace.error()};
      }

      if (!patch.patch.empty())
      {
        if (auto applied = extractor.apply(*workspace, patch); !applied)
        {
          return std::unexpected{applied.error()};
        }
      }

      auto request = ProcessRequest{};
      request.argv = oracleArgv(oracle);
      request.cwd = *workspace;
      request.environmentWhitelist = {
        "PATH", "HOME", "USER", "NIX_PATH", "IN_NIX_SHELL", "GSETTINGS_SCHEMA_DIR", "CPLUS_INCLUDE_PATH"};
      request.environment.emplace("BUILD_DIR", "/tmp/aobus-fleet-oracle-build");
      request.timeout = kDefaultOracleTimeout;
      auto authority = AuthorityPolicy{.id = "oracle",
                                       .filesystem = FilesystemAuthority::WritableCopy,
                                       .network = NetworkAuthority::Off,
                                       .contextView = ContextView::Full};
      auto process =
        NamespaceRunner{context.processRunner}.run(context.realRepo, *workspace, authority, std::move(request));
      auto log = process.standardOutput + process.standardError;
      return OracleEvidence{
        .oracleId = oracle.id,
        .oracleVersion = oracleVersion(oracle, context.immutableBase),
        .property = oracle.property,
        .passed = process.status == ProcessStatus::Exited && process.exitCode == 0,
        .infrastructureError =
          process.status == ProcessStatus::LaunchFailed || process.status == ProcessStatus::TimedOut,
        .exitCode = process.exitCode,
        .knownGaps = oracle.knownGaps,
        .log = std::move(log),
      };
    }

    RouteKey makeRoute(ResolvedPhase const& phase)
    {
      return RouteKey{
        .agentId = phase.agent.id,
        .modelVersion = phase.agent.model,
        .harness = "aobus-fleet/v1",
        .engine = phase.binding.engine,
        .oracleId = phase.optOracle ? phase.optOracle->id : "none",
        .oracleVersion = phase.optOracle ? "resolved-at-run" : "none",
        .authority =
          std::string{toString(phase.authority.filesystem)} + "+net-" + std::string{toString(phase.authority.network)},
        .scopeRiskClass = std::ranges::any_of(phase.intent.scope,
                                              [](ScopeRule const& rule)
                                              {
                                                auto const value = rule.path.generic_string();
                                                return value.starts_with("include/") ||
                                                       value.starts_with("app/include/");
                                              })
                            ? "public"
                            : "private",
      };
    }

    std::string proposalReview(ResolvedPhase const& phase, ReviewManifest const& manifest)
    {
      auto out = std::ostringstream{};
      out << "# Fleet Review\n\n";
      out << "Status: **" << (manifest.failure == FailureReason::None ? "PROPOSAL" : "ESCALATION") << "**\n\n";
      out << "Invariant: " << phase.intent.invariant << "\n\n";
      out << manifest.summary << "\n\n";
      out << "The fleet has not applied this patch. The chair must review and validate it on the real tree.\n";
      return out.str();
    }

    std::string synthesisDossier(ResolvedPhase const& phase,
                                 std::vector<std::pair<std::string, std::string>> const& drafts,
                                 std::vector<std::pair<std::string, std::string>> const& challenges,
                                 std::vector<std::pair<std::string, std::string>> const& revisions)
    {
      auto out = std::ostringstream{};
      out << "# Fleet Synthesis Dossier\n\n";
      out << "Invariant: " << phase.intent.invariant << "\n\n";
      out << "The chair performs the final synthesis. Quarantined members are omitted.\n";
      auto emitRound = [&out](std::string_view heading, auto const& rows)
      {
        if (rows.empty())
        {
          return;
        }

        out << "\n## " << heading << "\n";

        for (auto const& [member, text] : rows)
        {
          out << "\n### " << member << "\n\n" << text << "\n";
        }
      };
      emitRound("Blind Drafts", drafts);
      emitRound("Challenges", challenges);
      emitRound("Self Revisions", revisions);
      return out.str();
    }

    Result<> writeCommonArtifacts(ArtifactStore const& store,
                                  ResolvedPhase const& phase,
                                  ReviewManifest const& manifest)
    {
      if (auto value = store.write("intent.yaml", emitIntent(phase.intent)); !value)
      {
        return value;
      }

      if (auto value = store.write("resolved.yaml", emitResolved(phase)); !value)
      {
        return value;
      }

      if (auto value = store.write("manifest.yaml", emitManifest(manifest)); !value)
      {
        return value;
      }

      if (auto value = store.write("evidence.yaml", emitEvidence(manifest)); !value)
      {
        return value;
      }

      if (manifest.optPatch)
      {
        if (auto value = store.write("patch", manifest.optPatch->patch); !value)
        {
          return value;
        }
      }

      if (auto value = store.append("trace.yaml",
                                    emitTraceEvent("phase-completed",
                                                   {{"phase-id", manifest.phaseId},
                                                    {"output-mode", std::string{toString(manifest.mode)}},
                                                    {"failure", std::string{toString(manifest.failure)}}}));
          !value)
      {
        return value;
      }

      return {};
    }

    bool isEscalation(ReviewManifest const& manifest)
    {
      return manifest.failure != FailureReason::None;
    }

    FailureReason failureReasonFor(Error const& error)
    {
      return error.code == Error::Code::InvalidState ? FailureReason::DependencyFailed : FailureReason::Infrastructure;
    }

    template<typename Function>
    async::Task<std::invoke_result_t<Function>> runOnWorker(async::Runtime* runtime, Function function)
    {
      co_await runtime->resumeOnWorker();
      co_return std::invoke(function);
    }

    template<typename Function>
    auto spawnWorker(async::Runtime& runtime, Function function)
    {
      return runtime.spawn(runOnWorker(&runtime, std::move(function)));
    }

    std::string authorityLabel(AuthorityPolicy const& authority)
    {
      return std::string{toString(authority.filesystem)} + "+net-" + std::string{toString(authority.network)};
    }

    void assignCandidateRoute(ReviewManifest& manifest, Candidate const& candidate)
    {
      manifest.route.agentId = candidate.agent.id;
      manifest.route.modelVersion = candidate.agent.model;
      manifest.route.authority = authorityLabel(candidate.authority);
    }

    GateFallback gateFallback(Registry const& registry)
    {
      auto fallback = GateFallback{};
      auto rule = registry.escalations.find(FailureReason::NoCandidate);

      if (rule == registry.escalations.end() || rule->second.action != EscalationAction::SwitchRoute)
      {
        return fallback;
      }

      if (auto optRoute = rule->second.optRoute; optRoute)
      {
        if (auto agent = registry.agents.find(*optRoute); agent != registry.agents.end())
        {
          fallback.optAgent = agent->second;
          fallback.rounds = rule->second.retryLimit;
        }
      }

      return fallback;
    }

    std::optional<GateRoundConfig> gateRoundConfig(ResolvedPhase const& phase,
                                                   EngineContext const& context,
                                                   GateFallback const& fallback,
                                                   std::size_t round,
                                                   std::size_t primaryRounds)
    {
      if (round <= primaryRounds)
      {
        return GateRoundConfig{.agent = phase.agent, .authority = phase.authority};
      }

      if (!fallback.optAgent)
      {
        return std::nullopt;
      }

      auto const& fallbackDefault = context.registry.authorities.at(fallback.optAgent->defaultAuthority);
      auto unrestricted = AuthorityPolicy{.id = "fallback-clamp",
                                          .filesystem = FilesystemAuthority::MutateRealTree,
                                          .network = NetworkAuthority::Full,
                                          .contextView = ContextView::Full};
      return GateRoundConfig{
        .agent = *fallback.optAgent,
        .authority = intersectAuthority(phase.authority, fallbackDefault, unrestricted),
      };
    }

    Result<std::optional<ReviewManifest>> evaluateGateBaseline(ArtifactStore const& store,
                                                               ResolvedPhase const& phase,
                                                               EngineContext const& context)
    {
      if (!phase.optOracle || phase.optOracle->baselinePolicy == BaselinePolicy::Skip)
      {
        return std::optional<ReviewManifest>{};
      }

      auto baselinePatch =
        PatchArtifact{.candidateId = "baseline", .patch = {}, .touchedFiles = {}, .addedLines = 0, .removedLines = 0};
      auto baseline =
        runOracle(*phase.optOracle, baselinePatch, phase, context, context.runRoot / ".baseline" / phase.intent.id);

      if (!baseline)
      {
        return std::unexpected{baseline.error()};
      }

      [[maybe_unused]] auto const ret = store.write("baseline.log", baseline->log);

      if (phase.optOracle->baselinePolicy != BaselinePolicy::RequireGreen || baseline->passed)
      {
        return std::optional<ReviewManifest>{};
      }

      auto manifest = ReviewManifest{
        .phaseId = phase.intent.id,
        .mode = OutputMode::Proposal,
        .failure = baseline->infrastructureError ? FailureReason::Infrastructure : FailureReason::OracleFailed,
        .optPatch = std::nullopt,
        .oracleEvidence = {*baseline},
        .riskEvidence = {},
        .route = makeRoute(phase),
        .summary = "The required baseline oracle is not green; no worker was launched."};
      manifest.route.oracleVersion = baseline->oracleVersion;

      if (auto written = writeCommonArtifacts(store, phase, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      [[maybe_unused]] auto const ret2 = store.write("review.md", proposalReview(phase, manifest));
      return std::optional<ReviewManifest>{std::move(manifest)};
    }

    Result<Candidate> runGateCandidate(ResolvedPhase const& phase,
                                       EngineContext const& context,
                                       SnapshotProvider& snapshot,
                                       PatchExtractor& extractor,
                                       NamespaceRunner& namespaceRunner,
                                       GateRoundConfig const& config,
                                       std::size_t round,
                                       std::size_t index,
                                       std::string const& feedback)
    {
      auto const id = std::format("r{}-c{}", round, index + 1);
      auto workspace =
        snapshot.createWorkspace(context.immutableBase, context.runRoot / ".work" / phase.intent.id / id);

      if (!workspace)
      {
        return std::unexpected{workspace.error()};
      }

      auto prompt = gatePrompt(phase, round, feedback);
      auto request = agentRequest(config.agent, phase.intent, *workspace, context.realRepo, std::move(prompt));
      auto worker = namespaceRunner.run(context.realRepo, *workspace, config.authority, std::move(request));
      auto patch = extractor.extract(*workspace, id);

      if (!patch)
      {
        return std::unexpected{patch.error()};
      }

      auto guard = PatchGuard::inspect(*patch, phase.intent.scope, phase.binding.gate.churnLines, rulerPaths(phase));
      return Candidate{.patch = std::move(*patch),
                       .worker = std::move(worker),
                       .guard = std::move(guard),
                       .agent = config.agent,
                       .authority = config.authority};
    }

    void writeCandidateArtifacts(ArtifactStore const& store, Candidate const& candidate)
    {
      auto const prefix = std::filesystem::path{"candidates"} / candidate.patch.candidateId;
      [[maybe_unused]] auto const ret = store.write(prefix / "patch", candidate.patch.patch);
      [[maybe_unused]] auto const ret2 =
        store.write(prefix / "worker.log", candidate.worker.standardOutput + candidate.worker.standardError);
      auto candidateManifest = std::format("schema: aobus-fleet-candidate/v1\nid: {}\nagent: {}\nprocess-status: "
                                           "{}\nexit-code: {}\nguard: {}\ndetail: {}\n",
                                           yamlScalar(candidate.patch.candidateId),
                                           yamlScalar(candidate.agent.id),
                                           toString(candidate.worker.status),
                                           candidate.worker.exitCode,
                                           candidate.guard.accepted ? "accepted" : "rejected",
                                           yamlScalar(candidate.guard.detail));
      [[maybe_unused]] auto const ret3 = store.write(prefix / "manifest.yaml", candidateManifest);
    }

    bool acceptedCandidate(Candidate const& candidate)
    {
      return candidate.worker.status == ProcessStatus::Exited && candidate.worker.exitCode == 0 &&
             candidate.guard.accepted;
    }

    std::vector<Candidate> runGateRound(ArtifactStore const& store,
                                        ResolvedPhase const& phase,
                                        EngineContext const& context,
                                        SnapshotProvider& snapshot,
                                        PatchExtractor& extractor,
                                        NamespaceRunner& namespaceRunner,
                                        GateRoundConfig const& config,
                                        std::size_t round,
                                        std::string const& feedback)
    {
      auto futures = std::vector<std::future<Result<Candidate>>>{};

      for (std::size_t index = 0; index < phase.binding.gate.fanout; ++index)
      {
        futures.push_back(
          spawnWorker(context.asyncRuntime,
                      [&, round, index, feedback, config] -> Result<Candidate>
                      {
                        return runGateCandidate(
                          phase, context, snapshot, extractor, namespaceRunner, config, round, index, feedback);
                      }));
      }

      auto accepted = std::vector<Candidate>{};

      for (auto& future : futures)
      {
        auto candidate = future.get();

        if (!candidate)
        {
          continue;
        }

        writeCandidateArtifacts(store, *candidate);

        if (acceptedCandidate(*candidate))
        {
          accepted.push_back(std::move(*candidate));
        }
      }

      return accepted;
    }

    void sortGateCandidates(std::vector<Candidate>& candidates)
    {
      std::ranges::sort(candidates,
                        [](Candidate const& lhs, Candidate const& rhs)
                        {
                          return std::tuple{lhs.patch.touchedFiles.size(),
                                            lhs.patch.addedLines + lhs.patch.removedLines,
                                            lhs.patch.candidateId} <
                                 std::tuple{rhs.patch.touchedFiles.size(),
                                            rhs.patch.addedLines + rhs.patch.removedLines,
                                            rhs.patch.candidateId};
                        });
    }

    Result<ReviewManifest> finishGateManifest(ArtifactStore const& store,
                                              ResolvedPhase const& phase,
                                              ReviewManifest manifest)
    {
      if (auto written = writeCommonArtifacts(store, phase, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      [[maybe_unused]] auto const ret = store.write("review.md", proposalReview(phase, manifest));
      return manifest;
    }

    Result<std::optional<ReviewManifest>> acceptWithoutOracle(ArtifactStore const& store,
                                                              ResolvedPhase const& phase,
                                                              Candidate const& candidate)
    {
      auto manifest = ReviewManifest{
        .phaseId = phase.intent.id,
        .mode = OutputMode::Advisory,
        .failure = FailureReason::None,
        .optPatch = candidate.patch,
        .oracleEvidence = {},
        .riskEvidence = {},
        .route = makeRoute(phase),
        .summary = "A guarded candidate was produced without an independent oracle.",
      };
      assignCandidateRoute(manifest, candidate);

      auto finished = finishGateManifest(store, phase, std::move(manifest));

      if (!finished)
      {
        return std::unexpected{finished.error()};
      }

      return *finished;
    }

    Result<std::optional<ReviewManifest>> acceptWithOracle(ArtifactStore const& store,
                                                           ResolvedPhase const& phase,
                                                           Candidate const& candidate,
                                                           OracleEvidence const& evidence)
    {
      auto manifest = ReviewManifest{
        .phaseId = phase.intent.id,
        .mode = OutputMode::Proposal,
        .failure = FailureReason::None,
        .optPatch = candidate.patch,
        .oracleEvidence = {evidence},
        .riskEvidence = {},
        .route = makeRoute(phase),
        .summary = "A guarded candidate passed the independent oracle.",
      };
      assignCandidateRoute(manifest, candidate);
      manifest.route.oracleVersion = evidence.oracleVersion;

      if (phase.optRiskOracle)
      {
        auto risk = runRiskOracle(*phase.optRiskOracle, *manifest.optPatch);
        manifest.riskEvidence.push_back(risk);

        if (risk.fired)
        {
          manifest.failure = FailureReason::RiskOracleFired;
          manifest.summary = "The primary oracle passed, but a deterministic risk oracle requires heavier review.";
        }
      }

      auto finished = finishGateManifest(store, phase, std::move(manifest));

      if (!finished)
      {
        return std::unexpected{finished.error()};
      }

      return std::optional<ReviewManifest>{std::move(*finished)};
    }

    Result<std::optional<ReviewManifest>> evaluateGateCandidates(ArtifactStore const& store,
                                                                 ResolvedPhase const& phase,
                                                                 EngineContext const& context,
                                                                 std::vector<Candidate>& candidates,
                                                                 std::string& feedback,
                                                                 bool& hadOracleFailure)
    {
      if (candidates.empty())
      {
        return std::optional<ReviewManifest>{};
      }

      sortGateCandidates(candidates);

      if (!phase.optOracle)
      {
        return acceptWithoutOracle(store, phase, candidates.front());
      }

      auto const count = std::min(phase.binding.gate.topK, candidates.size());

      for (std::size_t index = 0; index < count; ++index)
      {
        auto evidence = runOracle(*phase.optOracle,
                                  candidates[index].patch,
                                  phase,
                                  context,
                                  context.runRoot / ".oracle" / phase.intent.id / candidates[index].patch.candidateId);

        if (!evidence)
        {
          continue;
        }

        [[maybe_unused]] auto const ret = store.write(
          std::filesystem::path{"candidates"} / candidates[index].patch.candidateId / "oracle.log", evidence->log);

        if (evidence->passed)
        {
          return acceptWithOracle(store, phase, candidates[index], *evidence);
        }

        hadOracleFailure = true;
        feedback = evidence->log;
      }

      candidates.clear();
      return std::optional<ReviewManifest>{};
    }

    std::vector<std::pair<std::string, std::string>> runCouncilRound(EngineContext const& context,
                                                                     SnapshotProvider& snapshot,
                                                                     NamespaceRunner& namespaceRunner,
                                                                     ArtifactStore const& store,
                                                                     ResolvedPhase const& phase,
                                                                     std::vector<std::string> const& roster,
                                                                     std::string_view round,
                                                                     std::string const& sharedContext)
    {
      auto futures = std::vector<std::future<std::optional<std::pair<std::string, std::string>>>>{};

      for (auto const& memberId : roster)
      {
        futures.push_back(spawnWorker(
          context.asyncRuntime,
          [&, memberId, round, sharedContext] -> std::optional<std::pair<std::string, std::string>>
          {
            auto const& agent = context.registry.agents.at(memberId);
            auto workspace = snapshot.createWorkspace(
              context.immutableBase, context.runRoot / ".council" / phase.intent.id / std::string{round} / memberId);

            if (!workspace)
            {
              return std::nullopt;
            }

            auto prompt = std::format("Council round: {}\nInvariant: {}\nTask:\n{}\n{}\nReturn only your substantive "
                                      "analysis. Do not mutate the repository.\n",
                                      round,
                                      phase.intent.invariant,
                                      phase.intent.body,
                                      sharedContext);
            auto request = agentRequest(agent, phase.intent, *workspace, context.realRepo, std::move(prompt));
            auto authority = phase.authority;
            authority.filesystem = FilesystemAuthority::ReadOnly;
            auto result = namespaceRunner.run(context.realRepo, *workspace, authority, std::move(request));
            auto text = result.standardOutput;

            if (result.status != ProcessStatus::Exited || result.exitCode != 0 ||
                text.find_first_not_of(" \t\r\n") == std::string::npos)
            {
              return std::nullopt;
            }

            [[maybe_unused]] auto const ret = store.write(
              std::filesystem::path{"members"} / memberId / (std::string{round} + ".log"), text + result.standardError);
            return std::pair{memberId, std::move(text)};
          }));
      }

      auto output = std::vector<std::pair<std::string, std::string>>{};

      for (auto& future : futures)
      {
        if (auto optValue = future.get(); optValue)
        {
          output.push_back(std::move(*optValue));
        }
      }

      return output;
    }
  } // namespace

  Result<ReviewManifest> GateEngine::execute(ResolvedPhase const& phase, EngineContext const& context)
  {
    auto store = ArtifactStore{context.runRoot / phase.intent.id};
    auto snapshot = SnapshotProvider{context.processRunner};
    auto extractor = PatchExtractor{context.processRunner};
    auto namespaceRunner = NamespaceRunner{context.processRunner};
    auto feedback = std::string{};
    auto candidates = std::vector<Candidate>{};
    auto fallback = gateFallback(context.registry);
    auto hadGuardedCandidate = false;
    auto hadOracleFailure = false;
    auto baseline = evaluateGateBaseline(store, phase, context);

    if (!baseline)
    {
      return std::unexpected{baseline.error()};
    }

    if (*baseline)
    {
      return **baseline;
    }

    auto const primaryRounds = phase.binding.gate.maxRounds;

    for (std::size_t round = 1; round <= primaryRounds + fallback.rounds; ++round)
    {
      if (round > primaryRounds && hadGuardedCandidate)
      {
        break;
      }

      auto optConfig = gateRoundConfig(phase, context, fallback, round, primaryRounds);

      if (!optConfig)
      {
        break;
      }

      auto accepted =
        runGateRound(store, phase, context, snapshot, extractor, namespaceRunner, *optConfig, round, feedback);
      hadGuardedCandidate = hadGuardedCandidate || !accepted.empty();
      candidates.insert(
        candidates.end(), std::make_move_iterator(accepted.begin()), std::make_move_iterator(accepted.end()));
      auto acceptedManifest = evaluateGateCandidates(store, phase, context, candidates, feedback, hadOracleFailure);

      if (!acceptedManifest)
      {
        return std::unexpected{acceptedManifest.error()};
      }

      if (*acceptedManifest)
      {
        return **acceptedManifest;
      }
    }

    auto manifest = ReviewManifest{
      .phaseId = phase.intent.id,
      .mode = phase.optOracle ? OutputMode::Proposal : OutputMode::Advisory,
      .failure = hadOracleFailure ? FailureReason::OracleFailed : FailureReason::NoCandidate,
      .optPatch = std::nullopt,
      .oracleEvidence = {},
      .riskEvidence = {},
      .route = makeRoute(phase),
      .summary = hadOracleFailure ? "Candidates exhausted the oracle retry budget."
                                  : "No candidate survived the configured gate and route-switch budget.",
    };

    auto finished = finishGateManifest(store, phase, std::move(manifest));

    if (!finished)
    {
      return std::unexpected{finished.error()};
    }

    return *finished;
  }

  Result<ReviewManifest> SynthesisEngine::execute(ResolvedPhase const& phase, EngineContext const& context)
  {
    auto store = ArtifactStore{context.runRoot / phase.intent.id};
    auto snapshot = SnapshotProvider{context.processRunner};
    auto namespaceRunner = NamespaceRunner{context.processRunner};
    auto const baseline = TreeCanary::fingerprint(context.immutableBase);

    if (!baseline)
    {
      return std::unexpected{baseline.error()};
    }

    auto roster = phase.binding.synthesis.roster;
    auto drafts = runCouncilRound(
      context, snapshot, namespaceRunner, store, phase, roster, "r1", "You cannot see other members' drafts.");

    if (drafts.size() < phase.binding.synthesis.quorum)
    {
      auto manifest = ReviewManifest{.phaseId = phase.intent.id,
                                     .mode = OutputMode::Advisory,
                                     .failure = FailureReason::QuorumFailed,
                                     .optPatch = std::nullopt,
                                     .oracleEvidence = {},
                                     .riskEvidence = {},
                                     .route = makeRoute(phase),
                                     .summary = "Council draft quorum failed."};

      if (auto written = writeCommonArtifacts(store, phase, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      [[maybe_unused]] auto const ret12 = store.write("dossier.md", synthesisDossier(phase, drafts, {}, {}));
      return manifest;
    }

    roster.clear();

    for (auto const& [member, ignored] : drafts)
    {
      [[maybe_unused]] auto const ret13 = ignored;
      roster.push_back(member);
    }

    auto challenges = std::vector<std::pair<std::string, std::string>>{};

    if (phase.binding.synthesis.depth >= CouncilDepth::Challenge)
    {
      auto peerContext = std::ostringstream{};
      peerContext << "Challenge the following peer drafts without assuming any is correct:\n";

      for (auto const& [member, draft] : drafts)
      {
        peerContext << "\n--- " << member << " ---\n" << draft << '\n';
      }

      challenges = runCouncilRound(context, snapshot, namespaceRunner, store, phase, roster, "r2", peerContext.str());
      roster.clear();

      for (auto const& [member, ignored] : challenges)
      {
        [[maybe_unused]] auto const ret14 = ignored;
        roster.push_back(member);
      }
    }

    auto revisions = std::vector<std::pair<std::string, std::string>>{};

    if (phase.binding.synthesis.depth == CouncilDepth::Full && roster.size() >= phase.binding.synthesis.quorum)
    {
      auto challengeContext = std::ostringstream{};
      challengeContext << "Revise your position after considering these challenges:\n";

      for (auto const& [member, challenge] : challenges)
      {
        challengeContext << "\n--- " << member << " ---\n" << challenge << '\n';
      }

      revisions =
        runCouncilRound(context, snapshot, namespaceRunner, store, phase, roster, "r3", challengeContext.str());
    }

    auto finalCount = std::size_t{0};

    if (phase.binding.synthesis.depth == CouncilDepth::Panel)
    {
      finalCount = drafts.size();
    }
    else if (phase.binding.synthesis.depth == CouncilDepth::Challenge)
    {
      finalCount = challenges.size();
    }
    else
    {
      finalCount = revisions.size();
    }

    auto manifest = ReviewManifest{
      .phaseId = phase.intent.id,
      .mode = OutputMode::Advisory,
      .failure = finalCount >= phase.binding.synthesis.quorum ? FailureReason::None : FailureReason::QuorumFailed,

      .optPatch = std::nullopt,
      .oracleEvidence = {},
      .riskEvidence = {},
      .route = makeRoute(phase),
      .summary = finalCount >= phase.binding.synthesis.quorum
                   ? "Council dossier reached quorum; chair synthesis remains required."
                   : "Council members were quarantined below quorum.",
    };

    if (auto written = writeCommonArtifacts(store, phase, manifest); !written)
    {
      return std::unexpected{written.error()};
    }

    [[maybe_unused]] auto const ret15 =
      store.write("dossier.md", synthesisDossier(phase, drafts, challenges, revisions));
    return manifest;
  }

  Result<std::vector<std::string>> Scheduler::order(std::vector<PhaseIntent> const& intents)
  {
    auto indegree = std::map<std::string, std::size_t, std::less<>>{};
    auto edges = std::map<std::string, std::vector<std::string>, std::less<>>{};

    for (auto const& intent : intents)
    {
      indegree.emplace(intent.id, 0);
    }

    for (auto const& intent : intents)
    {
      for (auto const& dependency : intent.dependsOn)
      {
        if (!indegree.contains(dependency))
        {
          return makeError(
            Error::Code::InvalidState, std::format("{}: dangling dependency '{}'", intent.id, dependency));
        }

        ++indegree[intent.id];
        edges[dependency].push_back(intent.id);
      }
    }

    auto ready = std::set<std::string>{};

    for (auto const& [id, count] : indegree)
    {
      if (count == 0)
      {
        ready.insert(id);
      }
    }

    auto result = std::vector<std::string>{};

    while (!ready.empty())
    {
      auto id = *ready.begin();
      ready.erase(ready.begin());
      result.push_back(id);

      for (auto const& dependent : edges[id])
      {
        if (--indegree[dependent] == 0)
        {
          ready.insert(dependent);
        }
      }
    }

    if (result.size() != intents.size())
    {
      return makeError(Error::Code::InvalidState, "scheduler: intent dependency graph contains a cycle");
    }

    return result;
  }

  namespace
  {
    struct WorkspaceCleanup final
    {
      IProcessRunner& runner;
      std::filesystem::path root;

      WorkspaceCleanup(IProcessRunner& runnerRef, std::filesystem::path rootPath)
        : runner{runnerRef}, root{std::move(rootPath)}
      {
      }

      ~WorkspaceCleanup()
      {
        auto snapshots = SnapshotProvider{runner};
        snapshots.remove(root / ".work");
        snapshots.remove(root / ".oracle");
        snapshots.remove(root / ".baseline");
        snapshots.remove(root / ".council");
        snapshots.remove(root / ".base");
      }

      WorkspaceCleanup(WorkspaceCleanup const&) = delete;
      WorkspaceCleanup& operator=(WorkspaceCleanup const&) = delete;
      WorkspaceCleanup(WorkspaceCleanup&&) = delete;
      WorkspaceCleanup& operator=(WorkspaceCleanup&&) = delete;
    };

    struct ResolvedIntentSet final
    {
      std::map<std::string, PhaseIntent const*, std::less<>> byId;
      std::map<std::string, ResolvedPhase, std::less<>> resolvedById;
    };

    struct FleetScheduleState final
    {
      std::set<std::string> pending;
      std::map<std::string, bool, std::less<>> completed;
      RunSummary summary;
    };

    constexpr auto kGlobalConcurrency = std::size_t{4};

    Result<ResolvedIntentSet> resolveFleetIntents(Registry const& registry, std::vector<PhaseIntent> const& intents)
    {
      auto result = ResolvedIntentSet{};

      for (auto const& intent : intents)
      {
        result.byId.emplace(intent.id, &intent);
      }

      for (auto const& intent : intents)
      {
        auto resolved = resolvePhase(registry, intent);

        if (!resolved)
        {
          return makeError(Error::Code::IoError, resolved.error().message);
        }

        result.resolvedById.emplace(intent.id, std::move(*resolved));
      }

      return result;
    }

    Result<> appendFleetAudit(std::filesystem::path const& outputRoot, ReviewManifest const& recorded)
    {
      auto audit = std::format("schema: aobus-fleet-audit-event/v1\n"
                               "event: phase-completed\n"
                               "phase-id: {}\n"
                               "output-mode: {}\n"
                               "failure: {}\n"
                               "timestamp: {}\n",
                               yamlScalar(recorded.phaseId),
                               toString(recorded.mode),
                               toString(recorded.failure),
                               yamlScalar(utcTimestamp()));
      return ArtifactStore{outputRoot}.append("audit.yaml", audit);
    }

    RouteKey runRoute(ResolvedPhase const& resolved, std::filesystem::path const& base)
    {
      auto route = makeRoute(resolved);

      if (resolved.optOracle)
      {
        route.oracleVersion = oracleVersion(*resolved.optOracle, base);
      }

      return route;
    }

    Result<ReviewManifest> finishPhaseReview(std::filesystem::path const& outputRoot,
                                             ResolvedPhase const& resolved,
                                             ReviewManifest manifest)
    {
      auto store = ArtifactStore{outputRoot / manifest.phaseId};

      if (auto written = writeCommonArtifacts(store, resolved, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      [[maybe_unused]] auto const ret = store.write("review.md", proposalReview(resolved, manifest));
      return manifest;
    }

    Result<std::optional<ReviewManifest>> routePausedManifest(std::filesystem::path const& outputRoot,
                                                              ResolvedPhase const& resolved,
                                                              RouteKey route)
    {
      auto routePaused = RouteStore{outputRoot}.paused(route.canonical());

      if (!routePaused)
      {
        return makeError(Error::Code::IoError, routePaused.error().message);
      }

      if (!*routePaused)
      {
        return std::optional<ReviewManifest>{};
      }

      auto manifest = ReviewManifest{.phaseId = resolved.intent.id,
                                     .mode = OutputMode::Advisory,
                                     .failure = FailureReason::RoutePaused,
                                     .optPatch = std::nullopt,
                                     .oracleEvidence = {},
                                     .riskEvidence = {},
                                     .route = std::move(route),
                                     .summary = "The route breaker is paused; no worker was launched."};
      auto finished = finishPhaseReview(outputRoot, resolved, std::move(manifest));

      if (!finished)
      {
        return std::unexpected{finished.error()};
      }

      return std::optional<ReviewManifest>{std::move(*finished)};
    }

    std::size_t infrastructureRetryLimit(Registry const& registry)
    {
      auto rule = registry.escalations.find(FailureReason::Infrastructure);

      if (rule == registry.escalations.end() || rule->second.action != EscalationAction::Retry)
      {
        return 0;
      }

      return rule->second.retryLimit;
    }

    Result<std::unique_ptr<IEngine>> makeEngine(EngineKind engine)
    {
      if (engine == EngineKind::Gate)
      {
        return std::make_unique<GateEngine>();
      }

      if (engine == EngineKind::Synthesis)
      {
        return std::make_unique<SynthesisEngine>();
      }

      return makeError(Error::Code::NotSupported, "search engine is unsupported");
    }

    Result<ReviewManifest> runEngineWithRetries(Registry const& registry,
                                                std::filesystem::path const& outputRoot,
                                                ResolvedPhase const& resolved,
                                                EngineContext const& context,
                                                RouteKey route)
    {
      auto retryLimit = infrastructureRetryLimit(registry);
      auto lastError = Error{.code = Error::Code::IoError, .message = "engine did not run"};

      for (std::size_t attempt = 0; attempt <= retryLimit; ++attempt)
      {
        auto enginePtr = makeEngine(resolved.binding.engine);

        if (!enginePtr)
        {
          return std::unexpected{enginePtr.error()};
        }

        auto manifest = (*enginePtr)->execute(resolved, context);

        if (manifest)
        {
          return manifest;
        }

        lastError = manifest.error();

        if (failureReasonFor(lastError) != FailureReason::Infrastructure)
        {
          break;
        }

        if (attempt < retryLimit)
        {
          [[maybe_unused]] auto const ret = ArtifactStore{outputRoot / resolved.intent.id}.append(
            "trace.yaml",
            emitTraceEvent("infrastructure-retry",
                           {{"phase-id", resolved.intent.id},
                            {"attempt", std::to_string(attempt + 1)},
                            {"detail", lastError.message}}));
        }
      }

      auto manifest = ReviewManifest{.phaseId = resolved.intent.id,
                                     .mode = resolved.optOracle ? OutputMode::Proposal : OutputMode::Advisory,
                                     .failure = failureReasonFor(lastError),
                                     .optPatch = std::nullopt,
                                     .oracleEvidence = {},
                                     .riskEvidence = {},
                                     .route = std::move(route),
                                     .summary = lastError.message};
      return finishPhaseReview(outputRoot, resolved, std::move(manifest));
    }

    Result<ReviewManifest> executeFleetPhase(Registry const& registry,
                                             std::filesystem::path const& outputRoot,
                                             std::filesystem::path const& base,
                                             ResolvedPhase const& resolved,
                                             EngineContext const& context)
    {
      auto route = runRoute(resolved, base);
      auto optPaused = routePausedManifest(outputRoot, resolved, route);

      if (!optPaused)
      {
        return std::unexpected{optPaused.error()};
      }

      if (*optPaused)
      {
        return **optPaused;
      }

      return runEngineWithRetries(registry, outputRoot, resolved, context, std::move(route));
    }

    std::set<std::string> phaseRateKeys(Registry const& registry, ResolvedPhase const& resolved)
    {
      auto keys = std::set<std::string>{};

      if (resolved.binding.engine == EngineKind::Synthesis)
      {
        for (auto const& member : resolved.binding.synthesis.roster)
        {
          auto const& agent = registry.agents.at(member);
          keys.insert(agent.rateLimitKey.empty() ? agent.id : agent.rateLimitKey);
        }
      }
      else
      {
        keys.insert(resolved.agent.rateLimitKey.empty() ? resolved.agent.id : resolved.agent.rateLimitKey);
      }

      return keys;
    }

    bool dependenciesAreComplete(PhaseIntent const& intent, FleetScheduleState const& state)
    {
      return std::ranges::all_of(
        intent.dependsOn, [&](auto const& dependency) { return state.completed.contains(dependency); });
    }

    bool hasFailedDependency(PhaseIntent const& intent, FleetScheduleState const& state)
    {
      return std::ranges::any_of(
        intent.dependsOn, [&](auto const& dependency) { return !state.completed.at(dependency); });
    }

    std::vector<std::string> readyPhaseIds(ResolvedIntentSet const& resolved, FleetScheduleState const& state)
    {
      auto ready = std::vector<std::string>{};

      for (auto const& id : state.pending)
      {
        if (dependenciesAreComplete(*resolved.byId.at(id), state))
        {
          ready.push_back(id);
        }
      }

      return ready;
    }

    Result<> recordCompletedPhase(std::filesystem::path const& outputRoot,
                                  FleetScheduleState& state,
                                  std::string const& id,
                                  ReviewManifest manifest)
    {
      state.completed[id] = !isEscalation(manifest);
      state.summary.escalated = state.summary.escalated || isEscalation(manifest);

      if (auto audited = appendFleetAudit(outputRoot, manifest); !audited)
      {
        return std::unexpected{audited.error()};
      }

      state.summary.manifests.push_back(std::move(manifest));
      state.pending.erase(id);
      return {};
    }

    Result<> recordDependencyFailure(std::filesystem::path const& outputRoot,
                                     std::filesystem::path const& base,
                                     ResolvedIntentSet const& resolved,
                                     FleetScheduleState& state,
                                     std::string const& id)
    {
      auto const& phase = resolved.resolvedById.at(id);
      auto manifest = ReviewManifest{.phaseId = id,
                                     .mode = OutputMode::Advisory,
                                     .failure = FailureReason::DependencyFailed,
                                     .optPatch = std::nullopt,
                                     .oracleEvidence = {},
                                     .riskEvidence = {},
                                     .route = runRoute(phase, base),
                                     .summary = "A dependency did not produce a usable result."};
      auto finished = finishPhaseReview(outputRoot, phase, std::move(manifest));

      if (!finished)
      {
        return std::unexpected{finished.error()};
      }

      return recordCompletedPhase(outputRoot, state, id, std::move(*finished));
    }

    Result<std::vector<std::string>> runnableReadyPhases(std::filesystem::path const& outputRoot,
                                                         std::filesystem::path const& base,
                                                         ResolvedIntentSet const& resolved,
                                                         FleetScheduleState& state,
                                                         std::vector<std::string> const& ready)
    {
      auto runnable = std::vector<std::string>{};

      for (auto const& id : ready)
      {
        if (auto const& intent = *resolved.byId.at(id); !hasFailedDependency(intent, state))
        {
          runnable.push_back(id);
          continue;
        }

        if (auto recorded = recordDependencyFailure(outputRoot, base, resolved, state, id); !recorded)
        {
          return std::unexpected{recorded.error()};
        }
      }

      return runnable;
    }

    std::vector<std::string> nextRunnableBatch(Registry const& registry,
                                               ResolvedIntentSet const& resolved,
                                               std::vector<std::string>& runnable)
    {
      auto batch = std::vector<std::string>{};
      auto usedKeys = std::set<std::string>{};

      for (auto iterator = runnable.begin(); iterator != runnable.end() && batch.size() < kGlobalConcurrency;)
      {
        auto keys = phaseRateKeys(registry, resolved.resolvedById.at(*iterator));
        auto conflicts = std::ranges::any_of(keys, [&](auto const& key) { return usedKeys.contains(key); });

        if (conflicts)
        {
          ++iterator;
          continue;
        }

        usedKeys.insert(keys.begin(), keys.end());
        batch.push_back(*iterator);
        iterator = runnable.erase(iterator);
      }

      if (batch.empty())
      {
        batch.push_back(runnable.front());
        runnable.erase(runnable.begin());
      }

      return batch;
    }

    Result<> executeRunnableBatch(Registry const& registry,
                                  std::filesystem::path const& outputRoot,
                                  ResolvedIntentSet const& resolved,
                                  FleetScheduleState& state,
                                  EngineContext const& context,
                                  std::vector<std::string> const& batch)
    {
      auto futures = std::vector<std::future<std::pair<std::string, Result<ReviewManifest>>>>{};

      for (auto const& id : batch)
      {
        futures.push_back(spawnWorker(
          context.asyncRuntime,
          [&, id]
          {
            return std::pair{
              id,
              executeFleetPhase(registry, outputRoot, context.immutableBase, resolved.resolvedById.at(id), context)};
          }));
      }

      for (auto& future : futures)
      {
        auto [id, result] = future.get();

        if (!result)
        {
          return std::unexpected{result.error()};
        }

        if (auto recorded = recordCompletedPhase(outputRoot, state, id, std::move(*result)); !recorded)
        {
          return std::unexpected{recorded.error()};
        }
      }

      return {};
    }

    Result<RunSummary> executeFleetSchedule(Registry const& registry,
                                            std::filesystem::path const& outputRoot,
                                            std::vector<std::string> const& ordered,
                                            ResolvedIntentSet const& resolved,
                                            EngineContext const& context)
    {
      auto state = FleetScheduleState{.pending = {ordered.begin(), ordered.end()}, .completed = {}, .summary = {}};

      while (!state.pending.empty())
      {
        auto ready = readyPhaseIds(resolved, state);

        if (ready.empty())
        {
          return makeError(Error::Code::InvalidState, "scheduler made no dependency progress");
        }

        auto runnable = runnableReadyPhases(outputRoot, context.immutableBase, resolved, state, ready);

        if (!runnable)
        {
          return std::unexpected{runnable.error()};
        }

        while (!runnable->empty())
        {
          auto batch = nextRunnableBatch(registry, resolved, *runnable);

          if (auto executed = executeRunnableBatch(registry, outputRoot, resolved, state, context, batch); !executed)
          {
            return std::unexpected{executed.error()};
          }
        }
      }

      return state.summary;
    }

    Result<RunSummary> discardIfRealTreeChanged(TreeFingerprint const& before,
                                                std::filesystem::path const& repo,
                                                std::filesystem::path const& outputRoot,
                                                RunSummary summary)
    {
      auto after = TreeCanary::fingerprint(repo);

      if (!after)
      {
        return std::unexpected{after.error()};
      }

      if (before == *after)
      {
        return summary;
      }

      for (auto& manifest : summary.manifests)
      {
        manifest.failure = FailureReason::RealTreeChanged;
        manifest.optPatch.reset();
        manifest.summary = "The real-tree canary changed; all delegated results were discarded.";
        auto store = ArtifactStore{outputRoot / manifest.phaseId};
        [[maybe_unused]] auto const ret = store.write("patch", "");
        [[maybe_unused]] auto const ret2 = store.write("manifest.yaml", emitManifest(manifest));
      }

      summary.escalated = true;
      return summary;
    }
  } // namespace

  FleetRunner::FleetRunner(IProcessRunner& processRunner)
    : _processRunner{processRunner}
  {
  }

  Result<RunSummary> FleetRunner::run(Registry const& registry,
                                      std::vector<PhaseIntent> const& intents,
                                      std::filesystem::path const& repo,
                                      std::filesystem::path const& out)
  {
    auto canonicalRepo = std::filesystem::weakly_canonical(repo);
    auto canonicalOut = std::filesystem::absolute(out).lexically_normal();

    if (canonicalOut == canonicalRepo || canonicalOut.string().starts_with(canonicalRepo.string() + "/"))
    {
      return makeError(Error::Code::IoError, "artifact output must be outside the repository");
    }

    std::filesystem::create_directories(canonicalOut);
    auto lockPath = std::filesystem::temp_directory_path() /
                    std::format("aobus-fleet-{:016x}.lock", std::hash<std::string>{}(canonicalRepo.string()));

    if (auto lockFile = std::ofstream{lockPath, std::ios::app}; !lockFile.is_open())
    {
      return makeError(Error::Code::IoError, "cannot create repository lock file");
    }

    auto optLock = std::optional<boost::interprocess::file_lock>{};
    optLock.emplace(lockPath.c_str());

    if (!optLock->try_lock())
    {
      return makeError(Error::Code::IoError, "another fleet run holds the repository lock");
    }

    auto before = TreeCanary::fingerprint(canonicalRepo);

    if (!before)
    {
      return std::unexpected{before.error()};
    }

    auto base = SnapshotProvider{_processRunner}.createImmutableBase(canonicalRepo, canonicalOut / ".base");

    if (!base)
    {
      return std::unexpected{base.error()};
    }

    auto const cleanup = WorkspaceCleanup{_processRunner, canonicalOut};
    auto ordered = Scheduler::order(intents);

    if (!ordered)
    {
      return std::unexpected{ordered.error()};
    }

    auto resolved = resolveFleetIntents(registry, intents);

    if (!resolved)
    {
      return std::unexpected{resolved.error()};
    }

    auto callbackExecutor = async::ImmediateExecutor{};
    auto asyncRuntime = async::Runtime{callbackExecutor, 16};
    auto context = EngineContext{.realRepo = canonicalRepo,
                                 .immutableBase = *base,
                                 .runRoot = canonicalOut,
                                 .registry = registry,
                                 .processRunner = _processRunner,
                                 .asyncRuntime = asyncRuntime};
    auto summary = executeFleetSchedule(registry, canonicalOut, *ordered, *resolved, context);

    if (!summary)
    {
      return std::unexpected{summary.error()};
    }

    return discardIfRealTreeChanged(*before, canonicalRepo, canonicalOut, std::move(*summary));
  }
} // namespace ao::fleet
