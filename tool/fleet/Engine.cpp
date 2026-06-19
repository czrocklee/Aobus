// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Engine.h"

#include "Hash.h"
#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include "fleet/RouteStore.h"
#include "fleet/Serialization.h"
#include "fleet/Substrate.h"
#include <ao/Error.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <boost/interprocess/sync/file_lock.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
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
    };

    struct GateFallback final
    {
      std::optional<AgentDefinition> optAgent;
      std::size_t rounds = 0;
    };

    struct GateRoundConfig final
    {
      AgentDefinition agent;
    };

    struct CouncilArtifactInput final
    {
      std::string memberId;
      std::string_view round;
      AgentDefinition const& agent;
      std::string_view prompt;
      ProcessResult const* result = nullptr;
      std::filesystem::path const* workspace = nullptr;
      bool quarantined = true;
      std::string_view quarantineReason;
    };

    // Artifact paths keep the stable r1/r2/r3 names; the label is the self-describing round
    // position shown to members ("2 of 3 (cross-challenge)").
    struct CouncilRound final
    {
      std::string_view directory;
      std::string label;
    };

    using CouncilRows = std::vector<std::pair<std::string, std::string>>;
    using CouncilContexts = std::map<std::string, std::string, std::less<>>;

    constexpr std::size_t kCouncilFullRounds = 3;
    constexpr std::size_t kCouncilChallengeRounds = 2;
    constexpr std::size_t kCouncilPanelRounds = 1;

    std::string replaceAll(std::string value, std::string_view from, std::string_view to)
    {
      std::size_t cursor = std::size_t{};

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

    void appendScopeRules(std::ostream& out, std::vector<ScopeRule> const& scope)
    {
      for (auto const& rule : scope)
      {
        out << "- " << rule.path.generic_string() << ":";

        for (auto operation : rule.operations)
        {
          out << ' ' << toString(operation);
        }

        out << '\n';
      }
    }

    std::string gatePrompt(ResolvedPhase const& phase, std::size_t round, std::string_view feedback)
    {
      auto out = std::ostringstream{};
      out << "Invariant: " << phase.intent.invariant << "\n";
      out << "Task:\n" << phase.intent.body << "\n";
      out << "Allowed paths and operations:\n";
      appendScopeRules(out, phase.intent.scope);
      out << "Round: " << round << "\n";

      if (!feedback.empty())
      {
        out << "Previous independent validation feedback:\n" << feedback << '\n';
      }

      return out.str();
    }

    std::vector<std::filesystem::path> rulerPaths(ResolvedPhase const& phase, Registry const& registry)
    {
      // Non-negotiable self-protection core: an accepted patch must never rewrite the fleet
      // tool or its registry, otherwise a candidate could disarm the guard reviewing it. The
      // rest of the base set is policy and lives in the registry-level `ruler-paths:`.
      auto result = std::vector<std::filesystem::path>{"tool/fleet", "config/agent-fleet.yaml"};
      result.insert(result.end(), registry.rulerPaths.begin(), registry.rulerPaths.end());

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

    std::vector<std::string> argumentList(std::map<std::string, std::string, std::less<>> const& arguments,
                                          std::string_view name,
                                          std::vector<std::string> defaults)
    {
      auto const found = arguments.find(name);

      if (found == arguments.end())
      {
        return defaults;
      }

      auto values = std::vector<std::string>{};

      for (auto const part : std::views::split(std::string_view{found->second}, ','))
      {
        if (!part.empty())
        {
          values.emplace_back(std::string_view{part});
        }
      }

      return values;
    }

    // Deterministic path patterns shared by the risk oracles and the route-key risk class, so
    // the registry can tune them in one place without the two consumers drifting apart.
    struct PathClassifier final
    {
      std::vector<std::string> testPrefixes;
      std::vector<std::string> testSuffixes;
      std::vector<std::string> headerPrefixes;
      std::vector<std::string> headerSuffixes;

      static PathClassifier fromArguments(std::map<std::string, std::string, std::less<>> const& arguments)
      {
        return PathClassifier{
          .testPrefixes = argumentList(arguments, "test-paths", {"test/"}),
          .testSuffixes = argumentList(arguments, "test-suffixes", {"Test.cpp"}),
          .headerPrefixes = argumentList(arguments, "header-prefixes", {"include/", "app/include/"}),
          .headerSuffixes = argumentList(arguments, "header-suffixes", {".h", ".hpp"}),
        };
      }

      bool isTestPath(std::filesystem::path const& path) const
      {
        auto const value = path.generic_string();
        return std::ranges::any_of(testPrefixes, [&](auto const& prefix) { return value.starts_with(prefix); }) ||
               std::ranges::any_of(testSuffixes, [&](auto const& suffix) { return value.ends_with(suffix); });
      }

      bool isPublicSurface(std::filesystem::path const& path) const
      {
        auto const value = path.generic_string();
        return std::ranges::any_of(headerPrefixes, [&](auto const& prefix) { return value.starts_with(prefix); });
      }

      bool isPublicHeader(std::filesystem::path const& path) const
      {
        auto const value = path.generic_string();
        return isPublicSurface(path) &&
               std::ranges::any_of(headerSuffixes, [&](auto const& suffix) { return value.ends_with(suffix); });
      }
    };

    std::string oracleVersion(OracleDefinition const& oracle, std::filesystem::path const& base)
    {
      auto hash = Fnv1a64{};
      hash.mix("aobus-fleet/v1");
      hash.mix(oracle.id);
      hash.mix(toString(oracle.runner));
      hash.mix(oracle.property);

      for (auto const& [name, value] : oracle.arguments)
      {
        hash.mix(name);
        hash.mix(value);
      }

      for (auto const& path : oracle.rulerPaths)
      {
        hash.mix(path.generic_string());

        if (auto const absolute = base / path; std::filesystem::is_regular_file(absolute))
        {
          hash.mixFile(absolute);
        }
        else if (std::filesystem::is_directory(absolute))
        {
          if (auto fingerprint = TreeCanary::fingerprint(absolute); fingerprint)
          {
            hash.mix(fingerprint->value);
          }
        }
      }

      return hash.hex();
    }

    // Sandbox-side mount point for the host-persistent oracle build tree. /tmp is a tmpfs in
    // the namespace, so without this bind every oracle invocation would rebuild from scratch
    // into RAM and the build would not even survive into the next command.
    constexpr auto kOracleBuildMount = std::string_view{"/tmp/build"};
    constexpr auto kOracleBuildDir = std::string_view{"/tmp/build/debug"};

    // Each oracle is a short command sequence executed in the same sandbox; the first failing
    // command terminates the sequence and provides the oracle outcome. Test oracles build
    // incrementally first so the candidate patch is actually compiled before tests run.
    std::vector<std::vector<std::string>> oracleCommands(OracleDefinition const& oracle)
    {
      switch (auto const buildDir = std::string{kOracleBuildDir}; oracle.runner)
      {
        case OracleRunner::TestAll: return {{"./ao", "check"}};
        case OracleRunner::TestCore:
        case OracleRunner::TestGtk:
        {
          auto const gtk = oracle.runner == OracleRunner::TestGtk;
          auto tests =
            std::vector<std::string>{"./ao", "test", "--no-build", "--path", buildDir, gtk ? "--gtk" : "--core"};

          if (auto filter = oracle.arguments.find("filter"); filter != oracle.arguments.end())
          {
            tests.push_back(filter->second);
          }

          // ao build configures the tree when needed and builds only the suite target without
          // running anything; ao test then runs only the requested suite against that tree.
          return {{"./ao", "build", "--target", gtk ? "ao_gtk_test" : "ao_core_test"}, std::move(tests)};
        }
        case OracleRunner::TestAsan: return {{"./ao", "check", "--asan"}};
        case OracleRunner::TestTsan: return {{"./ao", "check", "--tsan"}};
        case OracleRunner::TidyClean:
        {
          auto result = std::vector<std::string>{"./ao", "tidy"};

          if (auto scope = oracle.arguments.find("scope"); scope != oracle.arguments.end())
          {
            result.push_back(scope->second);
          }

          return {std::move(result)};
        }
        case OracleRunner::BuildDebug: return {{"./ao", "build", "--target", "aobus-gtk"}};
        case OracleRunner::TestDelta:
        case OracleRunner::PublicSignatureDelta: return {};
      }

      return {};
    }

    // Sanitizer builds need their own tree: mixing -fsanitize flags into the cached plain
    // debug configuration would silently rebuild everything and corrupt later oracles.
    std::string_view oracleBuildDir(OracleRunner runner)
    {
      switch (runner)
      {
        case OracleRunner::TestAsan: return "/tmp/build/debug-asan";
        case OracleRunner::TestTsan: return "/tmp/build/debug-tsan";
        default: return kOracleBuildDir;
      }
    }

    RiskEvidence runRiskOracle(OracleDefinition const& oracle, PatchArtifact const& patch)
    {
      auto result = RiskEvidence{.oracleId = oracle.id, .fired = false, .detail = {}};
      auto const classifier = PathClassifier::fromArguments(oracle.arguments);

      if (oracle.runner == OracleRunner::TestDelta)
      {
        result.fired = std::ranges::none_of(
          patch.touchedFiles, [&](TouchedFile const& touched) { return classifier.isTestPath(touched.path); });
        result.detail = result.fired ? "patch changes no registered test path" : "test path changed";
      }
      else if (oracle.runner == OracleRunner::PublicSignatureDelta)
      {
        result.fired = std::ranges::any_of(
          patch.touchedFiles, [&](TouchedFile const& touched) { return classifier.isPublicHeader(touched.path); });
        result.detail = result.fired ? "public header changed" : "no public header changed";
      }

      return result;
    }

    constexpr auto kDefaultOracleTimeout = std::chrono::minutes{30};

    Result<OracleEvidence> runOracle(OracleDefinition const& oracle,
                                     PatchArtifact const& patch,
                                     ResolvedPhase const& phase,
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

      // Persistent host-side build tree per phase: bind-mounted over the sandbox tmpfs so
      // incremental state survives across rounds and candidates of the same phase.
      auto const hostBuildRoot = context.runRoot / ".oracle-build" / phase.intent.id;
      // shell.nix pins CCACHE_DIR to <repo>/.cache/ccache; the workspace copy excludes .cache,
      // so bind the real cache into the virtualized repository path for full compile reuse.
      auto const hostCcache = context.realRepo / ".cache" / "ccache";
      auto mountError = std::error_code{};
      std::filesystem::create_directories(hostBuildRoot, mountError);
      std::filesystem::create_directories(hostCcache, mountError);

      if (mountError)
      {
        return makeError(Error::Code::IoError, mountError.message());
      }

      auto mounts = SandboxMounts{
        .writableBinds = {{hostBuildRoot, std::filesystem::path{kOracleBuildMount}}, {hostCcache, hostCcache}},
        .bindHome = false};
      auto commands = oracleCommands(oracle);

      if (commands.empty())
      {
        commands.emplace_back();
      }

      auto process = ProcessResult{};
      auto log = std::string{};

      for (auto& argv : commands)
      {
        auto request = ProcessRequest{};
        request.argv = std::move(argv);
        request.cwd = *workspace;
        // IN_NIX_SHELL is deliberately absent: the ao portal then re-enters nix-shell
        // inside the namespace and reconstructs the full build environment, which is
        // far too large (NIX_CC, PKG_CONFIG_PATH, ...) to allowlist reliably here.
        request.environmentWhitelist = {"PATH", "HOME", "USER", "NIX_PATH"};
        request.environment.emplace("BUILD_DIR", std::string{oracleBuildDir(oracle.runner)});
        request.timeout = oracle.optTimeout.value_or(std::chrono::milliseconds{kDefaultOracleTimeout});
        process = NamespaceRunner{context.processRunner}.run(context.realRepo, *workspace, mounts, std::move(request));
        log += process.standardOutput + process.standardError;

        if (process.status != ProcessStatus::Exited || process.exitCode != 0)
        {
          break;
        }
      }

      return OracleEvidence{
        .oracleId = oracle.id,
        .oracleVersion = context.oracleVersions.at(oracle.id),
        .property = oracle.property,
        .passed = process.status == ProcessStatus::Exited && process.exitCode == 0,
        .infrastructureError =
          process.status == ProcessStatus::LaunchFailed || process.status == ProcessStatus::TimedOut,
        .exitCode = process.exitCode,
        .knownGaps = oracle.knownGaps,
        .log = std::move(log),
      };
    }

    RouteKey makeRoute(ResolvedPhase const& phase, EngineContext const& context)
    {
      auto const classifier = phase.optRiskOracle ? PathClassifier::fromArguments(phase.optRiskOracle->arguments)
                                                  : PathClassifier::fromArguments({});
      return RouteKey{
        .agentId = phase.agent.id,
        .modelVersion = phase.agent.modelVersion(),
        // The CLI harness adapter is part of the competence key (design doc section 13.1):
        // statistics must not survive an agent moving to a different adapter.
        .harness = phase.agent.harness,
        .engine = phase.binding.engine,
        .oracleId = phase.optOracle ? phase.optOracle->id : "none",
        // Versions are precomputed once per run from the immutable base, so failure manifests
        // and breaker queries always agree on the same concrete route key.
        .oracleVersion = phase.optOracle ? context.oracleVersions.at(phase.optOracle->id) : "none",
        .scopeRiskClass =
          std::ranges::any_of(
            phase.intent.scope, [&](ScopeRule const& rule) { return classifier.isPublicSurface(rule.path); })
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

    // Resolves the registry escalation policy for a failed manifest so the chair can act on
    // the manifest alone; cleared again for successful phases.
    void applyEscalationAction(Registry const& registry, ReviewManifest& manifest)
    {
      manifest.optEscalationAction.reset();

      if (manifest.failure == FailureReason::None)
      {
        return;
      }

      if (auto rule = registry.escalations.find(manifest.failure); rule != registry.escalations.end())
      {
        manifest.optEscalationAction = rule->second.action;
      }
    }

    Result<> writeCommonArtifacts(ArtifactStore const& store,
                                  ResolvedPhase const& phase,
                                  Registry const& registry,
                                  ReviewManifest& manifest)
    {
      applyEscalationAction(registry, manifest);

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

    // Log-class artifacts (reviews, worker/oracle logs, dossiers) are diagnostics: losing
    // one must not fail the phase. The failure is surfaced on stderr and recorded in the
    // phase trace instead. Evidence-class artifacts (intent, resolved, manifest, evidence,
    // patch) keep fail-hard writes via writeCommonArtifacts and ArtifactStore::write.
    void writeOrWarn(ArtifactStore const& store, std::filesystem::path const& relativePath, std::string_view content)
    {
      auto const written = store.write(relativePath, content);

      if (written)
      {
        return;
      }

      std::cerr << std::format("fleet: warning: cannot write artifact {}: {}\n",
                               (store.root() / relativePath).string(),
                               written.error().message);
      [[maybe_unused]] auto const traced =
        store.append("trace.yaml",
                     emitTraceEvent("artifact-write-failed",
                                    {{"path", relativePath.generic_string()}, {"error", written.error().message}}));
    }

    void appendOrWarn(ArtifactStore const& store, std::filesystem::path const& relativePath, std::string_view document)
    {
      if (auto const appended = store.append(relativePath, document); !appended)
      {
        std::cerr << std::format("fleet: warning: cannot append artifact {}: {}\n",
                                 (store.root() / relativePath).string(),
                                 appended.error().message);
      }
    }

    std::string councilResultYaml(CouncilArtifactInput const& input)
    {
      auto const* result = input.result;
      auto out = std::ostringstream{};
      out << "schema: aobus-fleet-member-run/v1\n";
      out << "member: " << yamlScalar(input.memberId) << "\n";
      out << "round: " << yamlScalar(input.round) << "\n";
      out << "agent: " << yamlScalar(input.agent.id) << "\n";
      out << "harness: " << yamlScalar(input.agent.harness) << "\n";
      out << "model: " << yamlScalar(input.agent.model) << "\n";
      out << "vendor: " << yamlScalar(input.agent.vendor) << "\n";
      out << "effort: " << (input.agent.effort.empty() ? std::string{"null"} : yamlScalar(input.agent.effort)) << "\n";
      out << "prompt-delivery: " << toString(input.agent.promptDelivery) << "\n";
      out << "workspace: " << (input.workspace != nullptr ? yamlScalar(input.workspace->string()) : std::string{"null"})
          << "\n";
      out << "status: " << (result != nullptr ? std::string{toString(result->status)} : "launch-failed") << "\n";
      out << "exit-code: " << (result != nullptr ? result->exitCode : -1) << "\n";
      out << "signal: " << (result != nullptr ? result->signal : 0) << "\n";
      out << "elapsed-ms: " << (result != nullptr ? result->elapsed.count() : 0) << "\n";
      out << "prompt-bytes: " << input.prompt.size() << "\n";
      out << "stdout-bytes: " << (result != nullptr ? result->standardOutput.size() : 0) << "\n";
      out << "stderr-bytes: " << (result != nullptr ? result->standardError.size() : 0) << "\n";
      out << "quarantined: " << (input.quarantined ? "true" : "false") << "\n";
      out << "quarantine-reason: "
          << (input.quarantineReason.empty() ? std::string{"null"} : yamlScalar(input.quarantineReason)) << "\n";
      return out.str();
    }

    void writeCouncilArtifacts(ArtifactStore const& store, CouncilArtifactInput const& input)
    {
      auto const base = std::filesystem::path{"members"} / input.memberId / std::string{input.round};
      writeOrWarn(store, base / "prompt.md", input.prompt);
      writeOrWarn(store, base / "stdout.txt", input.result != nullptr ? input.result->standardOutput : "");
      writeOrWarn(store, base / "stderr.txt", input.result != nullptr ? input.result->standardError : "");
      writeOrWarn(store, base / "result.yaml", councilResultYaml(input));
    }

    std::size_t councilRoundCount(CouncilDepth depth)
    {
      if (depth == CouncilDepth::Full)
      {
        return kCouncilFullRounds;
      }

      if (depth == CouncilDepth::Challenge)
      {
        return kCouncilChallengeRounds;
      }

      return kCouncilPanelRounds;
    }

    std::string councilScopeSection(PhaseIntent const& intent)
    {
      if (intent.scope.empty())
      {
        return {};
      }

      auto out = std::ostringstream{};
      out << "Scope (focus on these paths and operations):\n";
      appendScopeRules(out, intent.scope);
      return std::move(out).str();
    }

    std::string_view draftRoundContext(CouncilDepth depth)
    {
      if (depth == CouncilDepth::Panel)
      {
        return "Produce an independent draft. The chair synthesizes the council drafts into the final result.";
      }

      if (depth == CouncilDepth::Challenge)
      {
        return "Produce an independent draft. Peer members will challenge it, and the chair synthesizes the "
               "council output into the final result.";
      }

      return "Produce an independent draft. Peer members will challenge it, you will then revise it, and the "
             "chair synthesizes the council output into the final result.";
    }

    std::string challengeRoundHeading(CouncilDepth depth)
    {
      auto const destination =
        (depth == CouncilDepth::Full)
          ? std::string{"Your challenge is given to each draft's author for revision and to the chair "
                        "for synthesis."}
          : std::string{"Your challenge goes to the chair for synthesis."};
      return std::format("Challenge the peer drafts below without assuming any is correct.\n"
                         "Judge them against the task above and verify their claims against the repository; "
                         "identify errors, unsupported claims, and omissions.\n{}",
                         destination);
    }

    CouncilContexts commonCouncilContexts(std::vector<std::string> const& roster, std::string_view context)
    {
      auto contexts = CouncilContexts{};

      for (auto const& member : roster)
      {
        contexts.emplace(member, context);
      }

      return contexts;
    }

    CouncilContexts peerCouncilContexts(std::vector<std::string> const& roster,
                                        std::string_view heading,
                                        CouncilRows const& rows)
    {
      auto contexts = CouncilContexts{};

      for (auto const& recipient : roster)
      {
        auto out = std::ostringstream{};
        out << heading << '\n';

        for (auto const& [member, text] : rows)
        {
          if (member != recipient)
          {
            out << "\n--- " << member << " ---\n" << text << '\n';
          }
        }

        contexts.emplace(recipient, std::move(out).str());
      }

      return contexts;
    }

    CouncilContexts revisionCouncilContexts(std::vector<std::string> const& roster,
                                            CouncilRows const& drafts,
                                            CouncilRows const& challenges)
    {
      auto contexts = peerCouncilContexts(roster,
                                          "Revise your prior draft after considering the peer challenges below; "
                                          "verify their claims against the repository before accepting them.\n"
                                          "The revision is your final statement to the chair.",
                                          challenges);

      for (auto const& recipient : roster)
      {
        auto const find = [&recipient](CouncilRows const& rows)
        { return std::ranges::find(rows, recipient, &std::pair<std::string, std::string>::first); };
        auto const draft = find(drafts);
        auto const ownChallenge = find(challenges);
        auto prefix = std::string{};

        if (draft != drafts.end())
        {
          prefix += std::format("Your prior draft:\n{}\n\n", draft->second);
        }

        // The member's own challenge is restated as its own notes, never as peer review; without it
        // the stateless revision process would lose whatever the member discovered while challenging.
        if (ownChallenge != challenges.end())
        {
          prefix += std::format("Your own challenge notes from the previous round:\n{}\n\n", ownChallenge->second);
        }

        contexts[recipient] = prefix + contexts.at(recipient);
      }

      return contexts;
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

    Error errorFromCurrentException()
    {
      try
      {
        throw;
      }
      catch (std::exception const& exception)
      {
        return Error{.code = Error::Code::InvalidState, .message = std::string{"worker raised: "} + exception.what()};
      }
      catch (...)
      {
        return Error{.code = Error::Code::InvalidState, .message = "worker raised an unknown exception"};
      }
    }

    template<typename T>
    Result<T> awaitOutcome(std::future<T>& future)
    {
      try
      {
        return future.get();
      }
      catch (...)
      {
        return std::unexpected{errorFromCurrentException()};
      }
    }

    // Joins every future before returning, so workers can never outlive the references they
    // capture; a worker exception becomes a per-slot error instead of abandoning its siblings.
    template<typename T>
    std::vector<Result<T>> awaitAll(std::vector<std::future<T>>& futures)
    {
      auto outcomes = std::vector<Result<T>>{};
      outcomes.reserve(futures.size());

      for (auto& future : futures)
      {
        outcomes.push_back(awaitOutcome(future));
      }

      return outcomes;
    }

    void assignCandidateRoute(ReviewManifest& manifest, Candidate const& candidate)
    {
      manifest.route.agentId = candidate.agent.id;
      manifest.route.modelVersion = candidate.agent.modelVersion();
      manifest.route.harness = candidate.agent.harness;
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
                                                   GateFallback const& fallback,
                                                   std::size_t round,
                                                   std::size_t primaryRounds)
    {
      if (round <= primaryRounds)
      {
        return GateRoundConfig{.agent = phase.agent};
      }

      if (!fallback.optAgent)
      {
        return std::nullopt;
      }

      return GateRoundConfig{.agent = *fallback.optAgent};
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

      writeOrWarn(store, "baseline.log", baseline->log);

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
        .route = makeRoute(phase, context),
        .summary = "The required baseline oracle is not green; no worker was launched.",
        .optEscalationAction = std::nullopt};

      if (auto written = writeCommonArtifacts(store, phase, context.registry, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      writeOrWarn(store, "review.md", proposalReview(phase, manifest));
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
      // Agents keep their real $HOME so model CLIs can read credentials and configuration.
      auto worker = namespaceRunner.run(
        context.realRepo, *workspace, SandboxMounts{.writableBinds = {}, .bindHome = true}, std::move(request));
      auto patch = extractor.extract(*workspace, id);

      if (!patch)
      {
        return std::unexpected{patch.error()};
      }

      auto guard = PatchGuard::inspect(
        *patch, phase.intent.scope, phase.binding.gate.churnLines, rulerPaths(phase, context.registry));
      return Candidate{
        .patch = std::move(*patch), .worker = std::move(worker), .guard = std::move(guard), .agent = config.agent};
    }

    void writeCandidateArtifacts(ArtifactStore const& store, Candidate const& candidate)
    {
      auto const prefix = std::filesystem::path{"candidates"} / candidate.patch.candidateId;
      writeOrWarn(store, prefix / "patch", candidate.patch.patch);
      writeOrWarn(store, prefix / "worker.log", candidate.worker.standardOutput + candidate.worker.standardError);
      auto candidateManifest = std::format("schema: aobus-fleet-candidate/v1\nid: {}\nagent: {}\nprocess-status: "
                                           "{}\nexit-code: {}\nguard: {}\ndetail: {}\n",
                                           yamlScalar(candidate.patch.candidateId),
                                           yamlScalar(candidate.agent.id),
                                           toString(candidate.worker.status),
                                           candidate.worker.exitCode,
                                           candidate.guard.accepted ? "accepted" : "rejected",
                                           yamlScalar(candidate.guard.detail));
      writeOrWarn(store, prefix / "manifest.yaml", candidateManifest);
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

      // Reference captures (config, feedback, locals) are safe: awaitAll below joins every
      // worker before this frame unwinds.
      for (std::size_t index = 0; index < phase.binding.gate.fanout; ++index)
      {
        futures.push_back(
          spawnWorker(context.asyncRuntime,
                      [&, round, index] -> Result<Candidate>
                      {
                        return runGateCandidate(
                          phase, context, snapshot, extractor, namespaceRunner, config, round, index, feedback);
                      }));
      }

      auto accepted = std::vector<Candidate>{};

      for (auto& outcome : awaitAll(futures))
      {
        if (!outcome || !*outcome)
        {
          continue;
        }

        auto& candidate = **outcome;
        writeCandidateArtifacts(store, candidate);

        if (acceptedCandidate(candidate))
        {
          accepted.push_back(std::move(candidate));
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
                                              Registry const& registry,
                                              ReviewManifest manifest)
    {
      if (auto written = writeCommonArtifacts(store, phase, registry, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      writeOrWarn(store, "review.md", proposalReview(phase, manifest));
      return manifest;
    }

    Result<std::optional<ReviewManifest>> acceptWithoutOracle(ArtifactStore const& store,
                                                              ResolvedPhase const& phase,
                                                              EngineContext const& context,
                                                              Candidate const& candidate)
    {
      auto manifest = ReviewManifest{
        .phaseId = phase.intent.id,
        .mode = OutputMode::Advisory,
        .failure = FailureReason::None,
        .optPatch = candidate.patch,
        .oracleEvidence = {},
        .riskEvidence = {},
        .route = makeRoute(phase, context),
        .summary = "A guarded candidate was produced without an independent oracle.",
        .optEscalationAction = std::nullopt,
      };
      assignCandidateRoute(manifest, candidate);

      auto finished = finishGateManifest(store, phase, context.registry, std::move(manifest));

      if (!finished)
      {
        return std::unexpected{finished.error()};
      }

      return *finished;
    }

    Result<std::optional<ReviewManifest>> acceptWithOracle(ArtifactStore const& store,
                                                           ResolvedPhase const& phase,
                                                           EngineContext const& context,
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
        .route = makeRoute(phase, context),
        .summary = "A guarded candidate passed the independent oracle.",
        .optEscalationAction = std::nullopt,
      };
      assignCandidateRoute(manifest, candidate);

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

      auto finished = finishGateManifest(store, phase, context.registry, std::move(manifest));

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
        return acceptWithoutOracle(store, phase, context, candidates.front());
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

        writeOrWarn(store,
                    std::filesystem::path{"candidates"} / candidates[index].patch.candidateId / "oracle.log",
                    evidence->log);

        if (evidence->passed)
        {
          return acceptWithOracle(store, phase, context, candidates[index], *evidence);
        }

        hadOracleFailure = true;
        feedback = evidence->log;
      }

      candidates.clear();
      return std::optional<ReviewManifest>{};
    }

    CouncilRows runCouncilRound(EngineContext const& context,
                                SnapshotProvider& snapshot,
                                NamespaceRunner& namespaceRunner,
                                ArtifactStore const& store,
                                ResolvedPhase const& phase,
                                std::vector<std::string> const& roster,
                                CouncilRound const& round,
                                CouncilContexts const& memberContexts)
    {
      auto const scope = councilScopeSection(phase.intent);
      auto futures = std::vector<std::future<std::optional<std::pair<std::string, std::string>>>>{};

      // Member contexts can be megabytes of dossier text; capture them (and the other locals) by
      // reference — awaitAll below joins every member before this frame unwinds.
      for (auto const& memberId : roster)
      {
        futures.push_back(spawnWorker(
          context.asyncRuntime,
          [&, memberId] -> std::optional<std::pair<std::string, std::string>>
          {
            auto const& agent = context.registry.agents.at(memberId);
            auto const& memberContext = memberContexts.at(memberId);
            auto prompt = std::format("Council round: {}\nInvariant: {}\nTask:\n{}\n{}{}\nReturn only your "
                                      "substantive analysis.\n",
                                      round.label,
                                      phase.intent.invariant,
                                      phase.intent.body,
                                      scope,
                                      memberContext);
            auto workspace = snapshot.createWorkspace(
              context.immutableBase,
              context.runRoot / ".council" / phase.intent.id / std::string{round.directory} / memberId);

            if (!workspace)
            {
              writeCouncilArtifacts(store,
                                    CouncilArtifactInput{.memberId = memberId,
                                                         .round = round.directory,
                                                         .agent = agent,
                                                         .prompt = prompt,
                                                         .result = nullptr,
                                                         .workspace = nullptr,
                                                         .quarantined = true,
                                                         .quarantineReason = "workspace-create-failed"});
              return std::nullopt;
            }

            auto request = agentRequest(agent, phase.intent, *workspace, context.realRepo, prompt);
            auto result = namespaceRunner.run(
              context.realRepo, *workspace, SandboxMounts{.writableBinds = {}, .bindHome = true}, std::move(request));
            auto const emptyOutput = result.standardOutput.find_first_not_of(" \t\r\n") == std::string::npos;
            auto const quarantined = result.status != ProcessStatus::Exited || result.exitCode != 0 || emptyOutput;

            auto reason = std::string{};

            if (result.status != ProcessStatus::Exited)
            {
              reason = "process-not-exited";
            }
            else if (result.exitCode != 0)
            {
              reason = "non-zero-exit";
            }
            else if (emptyOutput)
            {
              reason = "empty-output";
            }

            writeCouncilArtifacts(store,
                                  CouncilArtifactInput{.memberId = memberId,
                                                       .round = round.directory,
                                                       .agent = agent,
                                                       .prompt = prompt,
                                                       .result = &result,
                                                       .workspace = &*workspace,
                                                       .quarantined = quarantined,
                                                       .quarantineReason = reason});

            if (quarantined)
            {
              return std::nullopt;
            }

            return std::pair{memberId, std::move(result.standardOutput)};
          }));
      }

      auto output = CouncilRows{};

      for (auto& outcome : awaitAll(futures))
      {
        if (outcome && *outcome)
        {
          output.push_back(std::move(**outcome));
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
    bool hadGuardedCandidate = false;
    bool hadOracleFailure = false;
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

      auto optConfig = gateRoundConfig(phase, fallback, round, primaryRounds);

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
      .route = makeRoute(phase, context),
      .summary = hadOracleFailure ? "Candidates exhausted the oracle retry budget."
                                  : "No candidate survived the configured gate and route-switch budget.",
      .optEscalationAction = std::nullopt,
    };

    auto finished = finishGateManifest(store, phase, context.registry, std::move(manifest));

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

    auto const depth = phase.binding.synthesis.depth;
    auto const rounds = councilRoundCount(depth);
    auto roster = phase.binding.synthesis.roster;
    auto drafts =
      runCouncilRound(context,
                      snapshot,
                      namespaceRunner,
                      store,
                      phase,
                      roster,
                      CouncilRound{.directory = "r1", .label = std::format("1 of {} (independent draft)", rounds)},
                      commonCouncilContexts(roster, draftRoundContext(depth)));

    if (drafts.size() < phase.binding.synthesis.quorum)
    {
      auto manifest = ReviewManifest{.phaseId = phase.intent.id,
                                     .mode = OutputMode::Advisory,
                                     .failure = FailureReason::QuorumFailed,
                                     .optPatch = std::nullopt,
                                     .oracleEvidence = {},
                                     .riskEvidence = {},
                                     .route = makeRoute(phase, context),
                                     .summary = "Council draft quorum failed.",
                                     .optEscalationAction = std::nullopt};

      if (auto written = writeCommonArtifacts(store, phase, context.registry, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      writeOrWarn(store, "dossier.md", synthesisDossier(phase, drafts, {}, {}));
      return manifest;
    }

    roster.clear();

    for (auto const& member : std::views::keys(drafts))
    {
      roster.push_back(member);
    }

    auto challenges = std::vector<std::pair<std::string, std::string>>{};

    if (depth >= CouncilDepth::Challenge)
    {
      challenges =
        runCouncilRound(context,
                        snapshot,
                        namespaceRunner,
                        store,
                        phase,
                        roster,
                        CouncilRound{.directory = "r2", .label = std::format("2 of {} (cross-challenge)", rounds)},
                        peerCouncilContexts(roster, challengeRoundHeading(depth), drafts));
      roster.clear();

      for (auto const& member : std::views::keys(challenges))
      {
        roster.push_back(member);
      }
    }

    auto revisions = std::vector<std::pair<std::string, std::string>>{};

    if (depth == CouncilDepth::Full && roster.size() >= phase.binding.synthesis.quorum)
    {
      revisions = runCouncilRound(context,
                                  snapshot,
                                  namespaceRunner,
                                  store,
                                  phase,
                                  roster,
                                  CouncilRound{.directory = "r3", .label = "3 of 3 (revision)"},
                                  revisionCouncilContexts(roster, drafts, challenges));
    }

    std::size_t finalCount = 0;

    if (depth == CouncilDepth::Panel)
    {
      finalCount = drafts.size();
    }
    else if (depth == CouncilDepth::Challenge)
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
      .route = makeRoute(phase, context),
      .summary = finalCount >= phase.binding.synthesis.quorum
                   ? "Council dossier reached quorum; chair synthesis remains required."
                   : "Council members were quarantined below quorum.",
      .optEscalationAction = std::nullopt,
    };

    if (auto written = writeCommonArtifacts(store, phase, context.registry, manifest); !written)
    {
      return std::unexpected{written.error()};
    }

    writeOrWarn(store, "dossier.md", synthesisDossier(phase, drafts, challenges, revisions));
    return manifest;
  }

  Result<> Scheduler::validate(std::vector<PhaseIntent> const& intents)
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

    std::size_t visited = std::size_t{};

    while (!ready.empty())
    {
      auto id = *ready.begin();
      ready.erase(ready.begin());
      ++visited;

      for (auto const& dependent : edges[id])
      {
        if (--indegree[dependent] == 0)
        {
          ready.insert(dependent);
        }
      }
    }

    if (visited != intents.size())
    {
      return makeError(Error::Code::InvalidState, "scheduler: intent dependency graph contains a cycle");
    }

    return {};
  }

  namespace
  {
    // Shared between the end-of-run cleanup and the start-of-run sweep that recovers from a
    // previous crashed run (the inter-process file lock guarantees no concurrent owner).
    void sweepTransientWorkspaces(IProcessRunner& runner, std::filesystem::path const& root)
    {
      auto snapshots = SnapshotProvider{runner};

      for (auto const* directory : {".work", ".oracle", ".baseline", ".council", ".oracle-build", ".base"})
      {
        snapshots.remove(root / directory);
      }
    }

    struct WorkspaceCleanup final
    {
      IProcessRunner& runner;
      std::filesystem::path root;

      WorkspaceCleanup(IProcessRunner& runnerRef, std::filesystem::path rootPath)
        : runner{runnerRef}, root{std::move(rootPath)}
      {
      }

      ~WorkspaceCleanup() { sweepTransientWorkspaces(runner, root); }

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

    Result<ReviewManifest> finishPhaseReview(std::filesystem::path const& outputRoot,
                                             ResolvedPhase const& resolved,
                                             Registry const& registry,
                                             ReviewManifest manifest)
    {
      auto store = ArtifactStore{outputRoot / manifest.phaseId};

      if (auto written = writeCommonArtifacts(store, resolved, registry, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      writeOrWarn(store, "review.md", proposalReview(resolved, manifest));
      return manifest;
    }

    Result<std::optional<ReviewManifest>> routePausedManifest(std::filesystem::path const& outputRoot,
                                                              ResolvedPhase const& resolved,
                                                              Registry const& registry,
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
                                     .summary = "The route breaker is paused; no worker was launched.",
                                     .optEscalationAction = std::nullopt};
      auto finished = finishPhaseReview(outputRoot, resolved, registry, std::move(manifest));

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
          appendOrWarn(ArtifactStore{outputRoot / resolved.intent.id},
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
                                     .summary = lastError.message,
                                     .optEscalationAction = std::nullopt};
      return finishPhaseReview(outputRoot, resolved, registry, std::move(manifest));
    }

    Result<ReviewManifest> executeFleetPhase(Registry const& registry,
                                             std::filesystem::path const& outputRoot,
                                             ResolvedPhase const& resolved,
                                             EngineContext const& context)
    {
      auto route = makeRoute(resolved, context);
      auto optPaused = routePausedManifest(outputRoot, resolved, registry, route);

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
                                     EngineContext const& context,
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
                                     .route = makeRoute(phase, context),
                                     .summary = "A dependency did not produce a usable result.",
                                     .optEscalationAction = std::nullopt};
      auto finished = finishPhaseReview(outputRoot, phase, context.registry, std::move(manifest));

      if (!finished)
      {
        return std::unexpected{finished.error()};
      }

      return recordCompletedPhase(outputRoot, state, id, std::move(*finished));
    }

    Result<std::vector<std::string>> runnableReadyPhases(std::filesystem::path const& outputRoot,
                                                         EngineContext const& context,
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

        if (auto recorded = recordDependencyFailure(outputRoot, context, resolved, state, id); !recorded)
        {
          return std::unexpected{recorded.error()};
        }
      }

      return runnable;
    }

    // Single-producer-per-worker queue announcing finished phase ids to the scheduler thread.
    struct CompletionQueue final
    {
      std::mutex mutex;
      std::condition_variable ready;
      std::deque<std::string> finished;

      void push(std::string id)
      {
        {
          auto const lock = std::scoped_lock{mutex};
          finished.push_back(std::move(id));
        }

        ready.notify_one();
      }

      std::string pop()
      {
        auto lock = std::unique_lock{mutex};
        ready.wait(lock, [this] { return !finished.empty(); });
        auto id = std::move(finished.front());
        finished.pop_front();
        return id;
      }
    };

    struct InFlightPhase final
    {
      std::future<Result<ReviewManifest>> future;
      std::set<std::string> rateKeys;
    };

    // Every pending phase whose dependencies completed and that is not already running.
    // Phases with a failed dependency are recorded as DependencyFailed, and the scan repeats
    // until that cascade reaches a fixed point.
    Result<std::vector<std::string>> collectRunnablePhases(std::filesystem::path const& outputRoot,
                                                           EngineContext const& context,
                                                           ResolvedIntentSet const& resolved,
                                                           FleetScheduleState& state,
                                                           std::map<std::string, InFlightPhase> const& inFlight)
    {
      while (true)
      {
        auto ready = std::vector<std::string>{};

        for (auto const& id : state.pending)
        {
          if (!inFlight.contains(id) && dependenciesAreComplete(*resolved.byId.at(id), state))
          {
            ready.push_back(id);
          }
        }

        auto const pendingBefore = state.pending.size();
        auto runnable = runnableReadyPhases(outputRoot, context, resolved, state, ready);

        if (!runnable || state.pending.size() == pendingBefore)
        {
          return runnable;
        }
      }
    }

    Result<RunSummary> executeFleetSchedule(Registry const& registry,
                                            std::filesystem::path const& outputRoot,
                                            ResolvedIntentSet const& resolved,
                                            EngineContext const& context)
    {
      auto state = FleetScheduleState{.pending = {}, .completed = {}, .summary = {}};

      for (auto const& id : std::views::keys(resolved.resolvedById))
      {
        state.pending.insert(id);
      }

      auto queue = CompletionQueue{};
      auto inFlight = std::map<std::string, InFlightPhase>{};
      auto activeKeys = std::set<std::string>{};
      auto runnable = std::vector<std::string>{};
      auto optFatal = std::optional<Error>{};

      auto noteFatal = [&](Error error)
      {
        if (!optFatal)
        {
          optFatal = std::move(error);
        }
      };

      auto refillRunnable = [&]
      {
        if (auto collected = collectRunnablePhases(outputRoot, context, resolved, state, inFlight); collected)
        {
          runnable = std::move(*collected);
        }
        else
        {
          noteFatal(collected.error());
          runnable.clear();
        }
      };

      auto dispatch = [&](std::string const& id, std::set<std::string> keys)
      {
        activeKeys.insert(keys.begin(), keys.end());
        auto future =
          spawnWorker(context.asyncRuntime,
                      [&context, &registry, &outputRoot, &resolved, &queue, id]
                      {
                        auto result = [&] -> Result<ReviewManifest>
                        {
                          try
                          {
                            return executeFleetPhase(registry, outputRoot, resolved.resolvedById.at(id), context);
                          }
                          catch (...)
                          {
                            return std::unexpected{errorFromCurrentException()};
                          }
                        }();
                        queue.push(id);
                        return result;
                      });
        inFlight.emplace(id, InFlightPhase{.future = std::move(future), .rateKeys = std::move(keys)});
      };

      auto fillSlots = [&]
      {
        if (optFatal)
        {
          return;
        }

        auto iterator = runnable.begin();

        while (iterator != runnable.end() && inFlight.size() < kGlobalConcurrency)
        {
          auto keys = phaseRateKeys(registry, resolved.resolvedById.at(*iterator));

          if (std::ranges::any_of(keys, [&](auto const& key) { return activeKeys.contains(key); }))
          {
            ++iterator;
          }
          else
          {
            dispatch(*iterator, std::move(keys));
            iterator = runnable.erase(iterator);
          }
        }
      };

      auto reapOne = [&]
      {
        auto const id = queue.pop();
        auto entry = inFlight.find(id);
        auto outcome = awaitOutcome(entry->second.future);

        for (auto const& key : entry->second.rateKeys)
        {
          activeKeys.erase(key);
        }

        inFlight.erase(entry);

        if (!outcome)
        {
          noteFatal(outcome.error());
        }
        else if (!*outcome)
        {
          noteFatal((*outcome).error());
        }
        else if (auto recorded = recordCompletedPhase(outputRoot, state, id, std::move(**outcome)); !recorded)
        {
          noteFatal(recorded.error());
        }
      };

      while (true)
      {
        refillRunnable();
        fillSlots();

        if (inFlight.empty())
        {
          break;
        }

        reapOne();
      }

      if (optFatal)
      {
        return std::unexpected{*optFatal};
      }

      if (!state.pending.empty())
      {
        return makeError(Error::Code::InvalidState, "scheduler made no dependency progress");
      }

      return state.summary;
    }

    Result<RunSummary> discardIfRealTreeChanged(TreeFingerprint const& before,
                                                std::filesystem::path const& repo,
                                                std::filesystem::path const& outputRoot,
                                                Registry const& registry,
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
        applyEscalationAction(registry, manifest);
        auto store = ArtifactStore{outputRoot / manifest.phaseId};

        // Evidence-class rewrites: an on-disk manifest that still advertises the discarded
        // result would be a lie, so failing to persist the discard is a hard error.
        if (auto cleared = store.write("patch", ""); !cleared)
        {
          return std::unexpected{cleared.error()};
        }

        if (auto written = store.write("manifest.yaml", emitManifest(manifest)); !written)
        {
          return std::unexpected{written.error()};
        }
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

    // Recover from a crashed previous run before fingerprinting and snapshotting.
    sweepTransientWorkspaces(_processRunner, canonicalOut);

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

    if (auto valid = Scheduler::validate(intents); !valid)
    {
      return std::unexpected{valid.error()};
    }

    auto resolved = resolveFleetIntents(registry, intents);

    if (!resolved)
    {
      return std::unexpected{resolved.error()};
    }

    // The immutable base cannot change for the rest of the run, so every oracle version is
    // resolved exactly once here; failure manifests and breaker queries then share one key.
    auto oracleVersions = std::map<std::string, std::string, std::less<>>{};

    for (auto const& [id, oracle] : registry.oracles)
    {
      oracleVersions.emplace(id, oracleVersion(oracle, *base));
    }

    // The context storage is declared before the runtime so that the runtime destructor —
    // which joins the worker pool — runs while everything a worker can reference (context,
    // resolved set, snapshots) is still alive, even if a future were ever abandoned.
    auto optContext = std::optional<EngineContext>{};
    auto callbackExecutor = async::ImmediateExecutor{};
    auto asyncRuntime = async::Runtime{callbackExecutor, 16};
    optContext.emplace(EngineContext{.realRepo = canonicalRepo,
                                     .immutableBase = *base,
                                     .runRoot = canonicalOut,
                                     .registry = registry,
                                     .processRunner = _processRunner,
                                     .asyncRuntime = asyncRuntime,
                                     .oracleVersions = std::move(oracleVersions)});
    auto summary = executeFleetSchedule(registry, canonicalOut, *resolved, *optContext);

    if (!summary)
    {
      return std::unexpected{summary.error()};
    }

    return discardIfRealTreeChanged(*before, canonicalRepo, canonicalOut, registry, std::move(*summary));
  }
} // namespace ao::fleet
