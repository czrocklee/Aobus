// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Engine.h"
#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include "test/unit/fleet/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::fleet::test
{
  TEST_CASE("Fleet runner - mock proposal advisory and full council traces", "[fleet][integration][engine]")
  {
    auto temp = TempDir{};
    auto const repo = tempPath(temp) / "repo";
    auto const out = tempPath(temp) / "artifacts";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "original\n");
    auto const buildScript =
      writeFile(temp, "repo/ao", "#!/bin/sh\ntest ! -e .git/poison && grep -Eq '^(original|updated)$' source.txt\n");
    std::filesystem::permissions(buildScript,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write | std::filesystem::perms::group_exec |
                                   std::filesystem::perms::group_read | std::filesystem::perms::others_exec |
                                   std::filesystem::perms::others_read);

    auto process = BoostProcessRunner{};
    auto init = process.run(ProcessRequest{
      .argv = {"git", "init", repo.string()},
      .cwd = tempPath(temp),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{10},
      .terminationGracePeriod = std::chrono::seconds{1},
    });
    REQUIRE(init.status == ProcessStatus::Exited);
    REQUIRE(init.exitCode == 0);

    auto registry = Registry{};
    registry.agents.emplace(
      "editor",
      AgentDefinition{.id = "editor",
                      .harness = "shell",
                      .model = "editor-v1",
                      .vendor = "mock",
                      .effort = {},
                      .argvTemplate = {"sh", "-c", "printf 'updated\\n' > source.txt; printf poison > .git/poison"},
                      .promptDelivery = PromptDelivery::Stdin,
                      .environmentWhitelist = {"PATH"},
                      .timeout = std::chrono::seconds{10},
                      .rateLimitKey = "mock-editor"});
    registry.agents.emplace("no-op",
                            AgentDefinition{.id = "no-op",
                                            .harness = "shell",
                                            .model = "no-op-v1",
                                            .vendor = "mock",
                                            .effort = {},
                                            .argvTemplate = {"true"},
                                            .promptDelivery = PromptDelivery::Stdin,
                                            .environmentWhitelist = {"PATH"},
                                            .timeout = std::chrono::seconds{10},
                                            .rateLimitKey = "mock-no-op"});

    for (auto const& id : {"member-a", "member-b", "member-c"})
    {
      registry.agents.emplace(
        id,
        AgentDefinition{.id = id,
                        .harness = "shell",
                        .model = std::string{id} + "-v1",
                        .vendor = "mock",
                        .effort = {},
                        .argvTemplate = {"sh",
                                         "-c",
                                         std::format("prompt=$(cat); case \"$prompt\" in "
                                                     "*\"Council round: 1 of\"*) kind=draft ;; "
                                                     "*\"Council round: 2 of\"*) kind=challenge ;; "
                                                     "*) kind=revision ;; esac; "
                                                     "mkdir -p .codex && printf state > "
                                                     ".codex/state && printf '{} stderr\\n' >&2 && "
                                                     "printf '{} %s\\n' \"$kind\"",
                                                     id,
                                                     id)},
                        .promptDelivery = PromptDelivery::Stdin,
                        .environmentWhitelist = {"PATH"},
                        .timeout = std::chrono::seconds{10},
                        .rateLimitKey = id});
    }

    registry.agents.emplace("member-fail",
                            AgentDefinition{.id = "member-fail",
                                            .harness = "shell",
                                            .model = "member-fail-v1",
                                            .vendor = "mock",
                                            .effort = {},
                                            .argvTemplate = {"sh", "-c", "printf 'member-fail stderr\\n' >&2; exit 9"},
                                            .promptDelivery = PromptDelivery::Stdin,
                                            .environmentWhitelist = {"PATH"},
                                            .timeout = std::chrono::seconds{10},
                                            .rateLimitKey = "member-fail"});

    registry.oracles.emplace("mock-build",
                             OracleDefinition{.id = "mock-build",
                                              .runner = OracleRunner::BuildDebug,
                                              .arguments = {},
                                              .property = "source contains the expected update",
                                              .knownGaps = {},
                                              .baselinePolicy = BaselinePolicy::RequireGreen,
                                              .rulerPaths = {"ao"},
                                              .optTimeout = std::nullopt});
    registry.bindings.emplace("proposal",
                              Binding{.taskKind = "proposal",
                                      .agent = "editor",
                                      .engine = EngineKind::Gate,
                                      .optOracle = "mock-build",
                                      .optRiskOracle = std::nullopt,
                                      .gate = {.fanout = 2, .topK = 1, .maxRounds = 1, .churnLines = 20},
                                      .synthesis = {}});
    registry.bindings.emplace("advisory",
                              Binding{.taskKind = "advisory",
                                      .agent = "editor",
                                      .engine = EngineKind::Gate,
                                      .optOracle = std::nullopt,
                                      .optRiskOracle = std::nullopt,
                                      .gate = {.fanout = 1, .topK = 1, .maxRounds = 1, .churnLines = 20},
                                      .synthesis = {}});
    registry.bindings.emplace("council",
                              Binding{.taskKind = "council",
                                      .agent = "member-a",
                                      .engine = EngineKind::Synthesis,
                                      .optOracle = std::nullopt,
                                      .optRiskOracle = std::nullopt,
                                      .gate = {},
                                      .synthesis = {.roster = {"member-a", "member-b", "member-c", "member-fail"},
                                                    .depth = CouncilDepth::Full,
                                                    .quorum = 2}});
    registry.bindings.emplace("fallback",
                              Binding{.taskKind = "fallback",
                                      .agent = "no-op",
                                      .engine = EngineKind::Gate,
                                      .optOracle = std::nullopt,
                                      .optRiskOracle = std::nullopt,
                                      .gate = {.fanout = 1, .topK = 1, .maxRounds = 1, .churnLines = 20},
                                      .synthesis = {}});
    registry.escalations.emplace(FailureReason::NoCandidate,
                                 EscalationRule{.reason = FailureReason::NoCandidate,
                                                .action = EscalationAction::SwitchRoute,
                                                .optRoute = "editor",
                                                .retryLimit = 1});

    auto makeIntent = [](std::string id, std::string taskKind)
    {
      return PhaseIntent{.id = std::move(id),
                         .taskKind = std::move(taskKind),
                         .invariant = "Produce the requested artifact within the declared scope.",
                         .scope = {ScopeRule{.path = "source.txt", .operations = {ScopeOperation::Modify}}},
                         .dependsOn = {},
                         .overrides = {},
                         .body = "Produce the mock result."};
    };
    auto fallbackIntent = makeIntent("fallback-phase", "fallback");
    fallbackIntent.dependsOn = {"proposal-phase"};
    auto intents = std::vector{makeIntent("proposal-phase", "proposal"),
                               makeIntent("advisory-phase", "advisory"),
                               makeIntent("council-phase", "council"),
                               std::move(fallbackIntent)};

    auto result = FleetRunner{process}.run(registry, intents, repo, out);

    REQUIRE(result);
    REQUIRE(result->manifests.size() == 4);

    SECTION("gate routes produce proposal and advisory review surfaces")
    {
      auto const proposal =
        std::ranges::find(result->manifests, std::string{"proposal-phase"}, &ReviewManifest::phaseId);
      auto const advisory =
        std::ranges::find(result->manifests, std::string{"advisory-phase"}, &ReviewManifest::phaseId);
      auto const council = std::ranges::find(result->manifests, std::string{"council-phase"}, &ReviewManifest::phaseId);
      auto const fallback =
        std::ranges::find(result->manifests, std::string{"fallback-phase"}, &ReviewManifest::phaseId);
      REQUIRE(proposal != result->manifests.end());
      REQUIRE(advisory != result->manifests.end());
      REQUIRE(council != result->manifests.end());
      REQUIRE(fallback != result->manifests.end());
      CHECK(proposal->mode == OutputMode::Proposal);
      CHECK(advisory->mode == OutputMode::Advisory);
      CHECK(council->mode == OutputMode::Advisory);
      CHECK(fallback->mode == OutputMode::Advisory);
      CHECK(fallback->route.agentId == "editor");
      CHECK(proposal->route.oracleVersion.size() == 16);
      CHECK(std::filesystem::exists(out / "proposal-phase" / "patch"));
      CHECK(std::filesystem::exists(out / "advisory-phase" / "review.md"));
      CHECK(std::filesystem::exists(out / "council-phase" / "dossier.md"));
    }

    SECTION("council artifacts preserve round-specific context and quarantine failed members")
    {
      auto const memberRoot = out / "council-phase" / "members" / "member-a";
      CHECK_FALSE(std::filesystem::exists(memberRoot / "r1.log"));
      auto const memberPrompt = readFile(memberRoot / "r1" / "prompt.md");
      CHECK(memberPrompt.find("Council round: 1 of 3 (independent draft)") != std::string::npos);
      CHECK(memberPrompt.find("Scope (focus on these paths and operations):\n- source.txt: modify") !=
            std::string::npos);
      CHECK(memberPrompt.find("you will then revise it") != std::string::npos);
      CHECK(memberPrompt.find("real repository") == std::string::npos);
      CHECK(memberPrompt.find("workspace is disposable") == std::string::npos);
      CHECK(memberPrompt.find("repository copy") == std::string::npos);
      CHECK(readFile(memberRoot / "r1" / "stdout.txt") == "member-a draft\n");
      CHECK(readFile(memberRoot / "r1" / "stderr.txt") == "member-a stderr\n");

      auto const challengePrompt = readFile(memberRoot / "r2" / "prompt.md");
      CHECK(challengePrompt.find("Council round: 2 of 3 (cross-challenge)") != std::string::npos);
      CHECK(challengePrompt.find("verify their claims against the repository") != std::string::npos);
      CHECK(challengePrompt.find("given to each draft's author for revision") != std::string::npos);
      CHECK(challengePrompt.find("member-a draft") == std::string::npos);
      CHECK(challengePrompt.find("member-b draft") != std::string::npos);
      CHECK(challengePrompt.find("member-c draft") != std::string::npos);

      auto const revisionPrompt = readFile(memberRoot / "r3" / "prompt.md");
      CHECK(revisionPrompt.find("Council round: 3 of 3 (revision)") != std::string::npos);
      CHECK(revisionPrompt.find("Your prior draft:\nmember-a draft") != std::string::npos);
      CHECK(revisionPrompt.find("Your own challenge notes from the previous round:\nmember-a challenge") !=
            std::string::npos);
      CHECK(revisionPrompt.find("--- member-a ---") == std::string::npos);
      CHECK(revisionPrompt.find("member-b challenge") != std::string::npos);
      CHECK(revisionPrompt.find("member-c challenge") != std::string::npos);

      auto const memberResult = readFile(memberRoot / "r1" / "result.yaml");
      CHECK(memberResult.find("schema: aobus-fleet-member-run/v1") != std::string::npos);
      CHECK(memberResult.find("authority:") == std::string::npos);
      CHECK(memberResult.find("quarantined: false") != std::string::npos);
      CHECK(memberResult.find("harness: \"shell\"") != std::string::npos);
      CHECK(memberResult.find("vendor: \"mock\"") != std::string::npos);
      CHECK(memberResult.find("effort: null") != std::string::npos);
      auto const failedRoot = out / "council-phase" / "members" / "member-fail" / "r1";
      CHECK(readFile(failedRoot / "stderr.txt") == "member-fail stderr\n");
      auto const failedResult = readFile(failedRoot / "result.yaml");
      CHECK(failedResult.find("exit-code: 9") != std::string::npos);
      CHECK(failedResult.find("quarantine-reason: \"non-zero-exit\"") != std::string::npos);
    }

    SECTION("artifacts use versioned YAML without obsolete authority fields")
    {
      auto const proposalManifest = readFile(out / "proposal-phase" / "manifest.yaml");
      CHECK(proposalManifest.find("schema: aobus-fleet-manifest/v1") != std::string::npos);
      CHECK(proposalManifest.find("resolved-at-run") == std::string::npos);
      CHECK(proposalManifest.find("escalation-action:") != std::string::npos);
      CHECK(proposalManifest.find("authority") == std::string::npos);
      auto const resolvedPhase = readFile(out / "proposal-phase" / "resolved.yaml");
      CHECK(resolvedPhase.find("schema: aobus-fleet-resolved/v1") != std::string::npos);
      CHECK(resolvedPhase.find("authority:") == std::string::npos);
      CHECK(resolvedPhase.find("harness: \"shell\"") != std::string::npos);
      CHECK(resolvedPhase.find("vendor: \"mock\"") != std::string::npos);
      CHECK(resolvedPhase.find("effort: null") != std::string::npos);
      CHECK(std::filesystem::exists(out / "proposal-phase" / "trace.yaml"));
      CHECK(std::filesystem::exists(out / "audit.yaml"));

      auto hasJsonArtifact = false;

      for (auto const& entry : std::filesystem::recursive_directory_iterator{out})
      {
        hasJsonArtifact =
          hasJsonArtifact || entry.path().extension() == ".json" || entry.path().extension() == ".jsonl";
      }

      CHECK_FALSE(hasJsonArtifact);
    }

    SECTION("delegated work never mutates the real repository")
    {
      auto realSource = std::ifstream{repo / "source.txt"};
      auto realValue = std::string{};
      std::getline(realSource, realValue);
      CHECK(realValue == "original");
      CHECK_FALSE(std::filesystem::exists(repo / ".codex"));
    }
  }

  namespace
  {
    AgentDefinition shellAgent(std::string id, std::string script)
    {
      auto model = id + "-v1";
      return AgentDefinition{.id = std::move(id),
                             .harness = "shell",
                             .model = std::move(model),
                             .vendor = "mock",
                             .effort = {},
                             .argvTemplate = {"sh", "-c", std::move(script)},
                             .promptDelivery = PromptDelivery::Stdin,
                             .environmentWhitelist = {"PATH"},
                             .timeout = std::chrono::seconds{20},
                             .rateLimitKey = {}};
    }

    Binding gateBinding(std::string taskKind, std::string agent, std::optional<std::string> optOracle)
    {
      return Binding{.taskKind = std::move(taskKind),
                     .agent = std::move(agent),
                     .engine = EngineKind::Gate,
                     .optOracle = std::move(optOracle),
                     .optRiskOracle = std::nullopt,
                     .gate = {.fanout = 1, .topK = 1, .maxRounds = 1, .churnLines = 20},
                     .synthesis = {}};
    }

    PhaseIntent schedulerIntent(std::string id, std::string taskKind, std::vector<std::string> dependsOn)
    {
      return PhaseIntent{.id = std::move(id),
                         .taskKind = std::move(taskKind),
                         .invariant = "Preserve source behavior.",
                         .scope = {ScopeRule{.path = "source.txt", .operations = {ScopeOperation::Modify}}},
                         .dependsOn = std::move(dependsOn),
                         .overrides = {},
                         .body = "Produce the mock result."};
    }
  } // namespace

  TEST_CASE("Fleet scheduler - a fatal phase error still joins and audits in-flight phases",
            "[fleet][integration][engine]")
  {
    auto temp = TempDir{};
    auto const repo = tempPath(temp) / "repo";
    auto const out = tempPath(temp) / "artifacts";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "original\n");
    auto process = BoostProcessRunner{};
    initGitRepo(process, repo, tempPath(temp));

    auto registry = Registry{};
    registry.agents.emplace("slow", shellAgent("slow", "sleep 1"));
    registry.agents.emplace("doomed", shellAgent("doomed", "true"));
    registry.bindings.emplace("slow-kind", gateBinding("slow-kind", "slow", std::nullopt));
    registry.bindings.emplace("doomed-kind", gateBinding("doomed-kind", "doomed", std::nullopt));

    auto intents =
      std::vector{schedulerIntent("slow-phase", "slow-kind", {}), schedulerIntent("doomed-phase", "doomed-kind", {})};

    // A directory squatting on the manifest path makes every manifest write for the doomed
    // phase fail, which is one of the genuinely fatal (non-manifest) error paths.
    std::filesystem::create_directories(out / "doomed-phase" / "manifest.yaml");

    auto result = FleetRunner{process}.run(registry, intents, repo, out);

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("cannot write") != std::string::npos);
    // The fatal error must not abandon the in-flight sibling: it ran to completion and was audited.
    CHECK(std::filesystem::exists(out / "slow-phase" / "manifest.yaml"));
    CHECK(readFile(out / "audit.yaml").find("slow-phase") != std::string::npos);
  }

  TEST_CASE("Fleet scheduler - dependency failures cascade through transitive dependents",
            "[fleet][integration][engine]")
  {
    auto temp = TempDir{};
    auto const repo = tempPath(temp) / "repo";
    auto const out = tempPath(temp) / "artifacts";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "original\n");
    auto process = BoostProcessRunner{};
    initGitRepo(process, repo, tempPath(temp));

    auto registry = Registry{};
    registry.agents.emplace("failing", shellAgent("failing", "exit 1"));
    registry.agents.emplace("steady", shellAgent("steady", "true"));
    registry.bindings.emplace("failing-kind", gateBinding("failing-kind", "failing", std::nullopt));
    registry.bindings.emplace("steady-kind", gateBinding("steady-kind", "steady", std::nullopt));
    registry.escalations.emplace(FailureReason::DependencyFailed,
                                 EscalationRule{.reason = FailureReason::DependencyFailed,
                                                .action = EscalationAction::ReturnChair,
                                                .optRoute = std::nullopt,
                                                .retryLimit = 0});

    auto intents = std::vector{schedulerIntent("phase-a", "failing-kind", {}),
                               schedulerIntent("phase-b", "steady-kind", {"phase-a"}),
                               schedulerIntent("phase-c", "steady-kind", {"phase-b"})};

    auto result = FleetRunner{process}.run(registry, intents, repo, out);

    REQUIRE(result);
    REQUIRE(result->manifests.size() == 3);
    CHECK(result->escalated);
    auto failureOf = [&](std::string const& id)
    {
      auto manifest = std::ranges::find(result->manifests, id, &ReviewManifest::phaseId);
      REQUIRE(manifest != result->manifests.end());
      return manifest->failure;
    };
    CHECK(failureOf("phase-a") == FailureReason::NoCandidate);
    CHECK(failureOf("phase-b") == FailureReason::DependencyFailed);
    CHECK(failureOf("phase-c") == FailureReason::DependencyFailed);
    // Failed manifests carry the registry escalation action for the chair to act on.
    auto const manifestB = std::ranges::find(result->manifests, std::string{"phase-b"}, &ReviewManifest::phaseId);
    CHECK(manifestB->optEscalationAction == EscalationAction::ReturnChair);
    CHECK(readFile(out / "phase-b" / "manifest.yaml").find("escalation-action: return-chair") != std::string::npos);
  }
} // namespace ao::fleet::test
