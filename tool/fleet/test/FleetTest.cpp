// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/fleet/Engine.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>
#include <ao/fleet/RouteStore.h>
#include <ao/fleet/Serialization.h>
#include <ao/fleet/Substrate.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::fleet::test
{
  namespace
  {
    class TempDirectory final
    {
    public:
      TempDirectory()
        : _path{std::filesystem::temp_directory_path() / makePhaseId()}
      {
        std::filesystem::create_directories(_path);
      }

      ~TempDirectory()
      {
        auto error = std::error_code{};
        std::filesystem::remove_all(_path, error);
      }

      TempDirectory(TempDirectory const&) = delete;
      TempDirectory& operator=(TempDirectory const&) = delete;
      TempDirectory(TempDirectory&&) = delete;
      TempDirectory& operator=(TempDirectory&&) = delete;

      std::filesystem::path const& path() const noexcept { return _path; }

      std::filesystem::path write(std::string const& name, std::string_view content) const
      {
        auto const result = _path / name;
        std::filesystem::create_directories(result.parent_path());
        auto output = std::ofstream{result, std::ios::binary | std::ios::trunc};
        output << content;
        return result;
      }

    private:
      std::filesystem::path _path;
    };

    std::string intentYaml(std::string_view id = "phase-a",
                           std::string_view dependency = "",
                           std::string_view path = "lib/audio/Player.cpp")
    {
      auto depends = dependency.empty() ? "[]" : std::format("[{}]", dependency);
      return std::format(R"(schema: aobus-fleet-intent/v1
id: {}
task-kind: implement-plan
invariant: Preserve behavior.
scope:
  - path: {}
    operations: [modify]
depends-on: {}
overrides: {{}}
body: |
  Implement the approved change.
)",
                         id,
                         path,
                         depends);
    }

    std::string readFile(std::filesystem::path const& path)
    {
      auto input = std::ifstream{path, std::ios::binary};
      return {std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
    }
  } // namespace

  TEST_CASE("Fleet model - authority intersection is subtractive", "[fleet][unit][model]")
  {
    auto const broad = AuthorityPolicy{.id = "broad",
                                       .filesystem = FilesystemAuthority::WritableCopy,
                                       .network = NetworkAuthority::Full,
                                       .contextView = ContextView::Full};
    auto const narrow = AuthorityPolicy{.id = "narrow",
                                        .filesystem = FilesystemAuthority::ReadOnly,
                                        .network = NetworkAuthority::Off,
                                        .contextView = ContextView::Minimal};

    auto const effective = intersectAuthority(broad, narrow, broad);

    CHECK(effective.filesystem == FilesystemAuthority::ReadOnly);
    CHECK(effective.network == NetworkAuthority::Off);
    CHECK(effective.contextView == ContextView::Minimal);
  }

  TEST_CASE("Fleet registry - production registry validates and resolves", "[fleet][unit][yaml]")
  {
    auto registry = loadRegistry(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");

    REQUIRE(registry);
    CHECK(registry->agents.contains("dspro"));
    CHECK(registry->oracles.contains("test-all"));
    CHECK(registry->bindings.at("implement-plan").engine == EngineKind::Gate);
    CHECK(registry->bindings.at("council-plan").engine == EngineKind::Synthesis);
  }

  TEST_CASE("Fleet registry - invalid references placeholders and search fail statically", "[fleet][unit][yaml]")
  {
    auto temp = TempDirectory{};
    auto const production = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");

    SECTION("unknown agent reference")
    {
      auto source = production;
      auto const position = source.find("agent: dspro");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"agent: dspro"}.size(), "agent: missing");
      auto result = loadRegistry(temp.write("missing-agent.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown agent") != std::string::npos);
    }

    SECTION("unknown argv placeholder")
    {
      auto source = production;
      auto const position = source.find("argv: [opencode");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"argv: [opencode"}.size(), "argv: [\"{bogus}\", opencode");
      auto result = loadRegistry(temp.write("placeholder.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown placeholder") != std::string::npos);
    }

    SECTION("search binding")
    {
      auto source = production;
      auto const position = source.find("engine: gate");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"engine: gate"}.size(), "engine: search");
      auto result = loadRegistry(temp.write("search.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unsupported search engine") != std::string::npos);
    }
  }

  TEST_CASE("Fleet resolution - authority override cannot elevate binding", "[fleet][unit][authority]")
  {
    auto registry = Registry{};
    registry.authorities.emplace("read",
                                 AuthorityPolicy{.id = "read",
                                                 .filesystem = FilesystemAuthority::ReadOnly,
                                                 .network = NetworkAuthority::Off,
                                                 .contextView = ContextView::Minimal});
    registry.authorities.emplace("write",
                                 AuthorityPolicy{.id = "write",
                                                 .filesystem = FilesystemAuthority::WritableCopy,
                                                 .network = NetworkAuthority::Full,
                                                 .contextView = ContextView::Full});
    registry.agents.emplace("worker",
                            AgentDefinition{.id = "worker",
                                            .vendor = "mock",
                                            .model = "mock-v1",
                                            .argvTemplate = {"true"},
                                            .promptDelivery = PromptDelivery::Stdin,
                                            .environmentWhitelist = {"PATH"},
                                            .credentialMounts = {},
                                            .timeout = std::chrono::seconds{1},
                                            .rateLimitKey = "mock",
                                            .defaultAuthority = "write"});
    registry.bindings.emplace("review",
                              Binding{.taskKind = "review",
                                      .agent = "worker",
                                      .engine = EngineKind::Synthesis,
                                      .optOracle = std::nullopt,
                                      .optRiskOracle = std::nullopt,
                                      .authority = "read",
                                      .gate = {},
                                      .synthesis = {.roster = {"worker"}, .depth = CouncilDepth::Panel, .quorum = 1}});
    auto overrides = IntentOverrides{};
    overrides.optAuthority = "write";

    auto intent = PhaseIntent{.id = "phase-a",
                              .taskKind = "review",
                              .invariant = "read only",
                              .scope = {},
                              .dependsOn = {},
                              .overrides = std::move(overrides),
                              .body = "review"};

    auto resolved = resolvePhase(registry, intent);

    REQUIRE(resolved);
    CHECK(resolved->authority.filesystem == FilesystemAuthority::ReadOnly);
    CHECK(resolved->authority.network == NetworkAuthority::Off);
    CHECK(resolved->authority.contextView == ContextView::Minimal);
  }

  TEST_CASE("Fleet intent - strict schema and round trip", "[fleet][unit][yaml]")
  {
    auto temp = TempDirectory{};

    SECTION("valid intent round trips")
    {
      auto first = loadIntent(temp.write("first.yaml", intentYaml()));
      REQUIRE(first);
      auto second = loadIntent(temp.write("second.yaml", emitIntent(*first)));
      REQUIRE(second);
      CHECK(second->id == first->id);
      CHECK(second->scope == first->scope);
      CHECK(second->body == first->body);
    }

    SECTION("unknown keys are rejected")
    {
      auto source = intentYaml();
      source += "surprise: true\n";
      auto result = loadIntent(temp.write("unknown.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown field") != std::string::npos);
    }

    SECTION("duplicate keys are rejected")
    {
      auto source = intentYaml();
      source += "body: duplicate\n";
      auto result = loadIntent(temp.write("duplicate.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("duplicate key") != std::string::npos);
    }

    SECTION("path traversal is rejected")
    {
      auto result = loadIntent(temp.write("traversal.yaml", intentYaml("phase-a", "", "../outside.cpp")));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("traversal") != std::string::npos);
    }

    SECTION("artifact ID traversal is rejected")
    {
      auto result = loadIntent(temp.write("id-traversal.yaml", intentYaml("../escape")));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unsafe") != std::string::npos);
    }

    SECTION("anchors are rejected")
    {
      auto source = intentYaml();
      source += "# &forbidden\n";
      auto result = loadIntent(temp.write("anchor.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("anchors") != std::string::npos);
    }

    SECTION("custom tags are rejected")
    {
      auto source = intentYaml();
      auto const position = source.find("Preserve behavior.");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"Preserve behavior."}.size(), "!secret Preserve behavior.");
      auto result = loadIntent(temp.write("tag.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("custom tags") != std::string::npos);
    }

    SECTION("document size is bounded")
    {
      auto result = loadIntent(temp.write("large.yaml", std::string((2 * 1024 * 1024) + 1, 'x')));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("2 MiB") != std::string::npos);
    }
  }

  TEST_CASE("Fleet intents - duplicate IDs and dangling dependencies fail", "[fleet][unit][scheduler]")
  {
    auto temp = TempDirectory{};

    SECTION("duplicate IDs")
    {
      auto paths = std::vector{temp.write("one.yaml", intentYaml()), temp.write("two.yaml", intentYaml())};
      auto result = loadIntents(paths);
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("duplicate intent id") != std::string::npos);
    }

    SECTION("dangling dependency")
    {
      auto paths = std::vector{temp.write("one.yaml", intentYaml("phase-a", "missing"))};
      auto result = loadIntents(paths);
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("dangling dependency") != std::string::npos);
    }
  }

  TEST_CASE("Fleet scheduler - stable dependency ordering and cycle rejection", "[fleet][unit][scheduler]")
  {
    auto first = PhaseIntent{.id = "a",
                             .taskKind = "test",
                             .invariant = "test",
                             .scope = {},
                             .dependsOn = {},
                             .overrides = {},
                             .body = "test"};
    auto second = PhaseIntent{.id = "b",
                              .taskKind = "test",
                              .invariant = "test",
                              .scope = {},
                              .dependsOn = {"a"},
                              .overrides = {},
                              .body = "test"};
    auto third = PhaseIntent{.id = "c",
                             .taskKind = "test",
                             .invariant = "test",
                             .scope = {},
                             .dependsOn = {"b"},
                             .overrides = {},
                             .body = "test"};

    SECTION("orders a DAG")
    {
      auto result = Scheduler::order({third, first, second});
      REQUIRE(result);
      CHECK(*result == std::vector<std::string>{"a", "b", "c"});
    }

    SECTION("rejects a cycle")
    {
      first.dependsOn = {"c"};
      auto result = Scheduler::order({first, second, third});
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("cycle") != std::string::npos);
    }
  }

  TEST_CASE("Fleet patch guard - enforces scope operation churn and rulers", "[fleet][unit][guard]")
  {
    auto patch = PatchArtifact{
      .candidateId = "candidate-a",
      .patch = "diff --git a/lib/audio/Player.cpp b/lib/audio/Player.cpp\n",
      .touchedFiles = {"lib/audio/Player.cpp"},
      .addedLines = 3,
      .removedLines = 2,
    };
    auto const scope = std::vector{ScopeRule{.path = "lib/audio/Player.cpp", .operations = {ScopeOperation::Modify}}};

    CHECK(PatchGuard::inspect(patch, scope, 10, {"tool/fleet"}).accepted);

    patch.touchedFiles = {"lib/audio/Other.cpp"};
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);

    patch.touchedFiles = {"lib/audio/Player.cpp"};
    patch.addedLines = 20;
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ChurnExceeded);

    patch.addedLines = 3;
    patch.touchedFiles = {"tool/fleet/src/Engine.cpp"};
    CHECK(
      PatchGuard::inspect(patch, {ScopeRule{"tool/fleet/src/Engine.cpp", {ScopeOperation::Modify}}}, 10, {"tool/fleet"})
        .failure == FailureReason::ScopeViolation);

    patch.touchedFiles = {"lib/audio/Player.cpp"};
    patch.patch += "old mode 100644\nnew mode 100755\n";
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);
  }

  TEST_CASE("Fleet tree canary - content mode and symlink targets affect the fingerprint", "[fleet][unit][canary]")
  {
    auto temp = TempDirectory{};
    auto const file = temp.write("source.txt", "first\n");
    std::filesystem::create_symlink("source.txt", temp.path() / "link");
    auto first = TreeCanary::fingerprint(temp.path());
    REQUIRE(first);

    temp.write("source.txt", "second\n");
    auto contentChanged = TreeCanary::fingerprint(temp.path());
    REQUIRE(contentChanged);
    CHECK(*contentChanged != *first);

    temp.write("source.txt", "first\n");
    std::filesystem::permissions(file, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    auto modeChanged = TreeCanary::fingerprint(temp.path());
    REQUIRE(modeChanged);
    CHECK(*modeChanged != *first);

    std::filesystem::permissions(file, std::filesystem::perms::owner_exec, std::filesystem::perm_options::remove);
    std::filesystem::remove(temp.path() / "link");
    std::filesystem::create_symlink("missing.txt", temp.path() / "link");
    auto linkChanged = TreeCanary::fingerprint(temp.path());
    REQUIRE(linkChanged);
    CHECK(*linkChanged != *first);
  }

  TEST_CASE("Fleet YAML stream - incomplete tail is recoverable", "[fleet][unit][yaml]")
  {
    auto temp = TempDirectory{};
    auto const path = temp.path() / "review-outcomes.yaml";
    auto document = std::string{"schema: aobus-fleet-review-outcome/v1\n"
                                "event: review-recorded\n"
                                "phase-id: \"phase-a\"\n"
                                "route-key: \"route-a\"\n"
                                "verdict: accept\n"
                                "reason: \"good\"\n"
                                "timestamp: \"2026-06-09T00:00:00Z\"\n"};
    REQUIRE(appendYamlDocument(path, document));
    {
      auto output = std::ofstream{path, std::ios::app | std::ios::binary};
      output << "---\nschema: aobus-fleet-review-outcome/v1\nphase-id:";
    }

    auto result = readReviewOutcomes(path);

    REQUIRE(result);
    REQUIRE(result->outcomes.size() == 1);
    CHECK(result->outcomes.front().verdict == ReviewVerdict::Accept);
    CHECK(result->trailingCorruption);
  }

  TEST_CASE("Fleet route store - breaker uses five-result window and reset", "[fleet][unit][route]")
  {
    auto temp = TempDirectory{};
    auto manifest = ReviewManifest{
      .phaseId = "phase-a",
      .mode = OutputMode::Proposal,
      .failure = FailureReason::None,
      .optPatch = std::nullopt,
      .oracleEvidence = {},
      .riskEvidence = {},
      .route = RouteKey{.agentId = "agent",
                        .modelVersion = "model",
                        .harness = "harness",
                        .engine = EngineKind::Gate,
                        .oracleId = "test",
                        .oracleVersion = "v1",
                        .authority = "copy",
                        .scopeRiskClass = "private"},
      .summary = "test",
    };
    auto store = RouteStore{temp.path()};

    for (auto index = 0; index < 5; ++index)
    {
      manifest.phaseId = std::format("phase-{}", index);
      temp.write(manifest.phaseId + "/manifest.yaml", emitManifest(manifest));
      auto const verdict = index < 3 ? ReviewVerdict::Reject : ReviewVerdict::Accept;
      REQUIRE(store.record(manifest.phaseId, verdict, "test outcome"));
    }

    auto pausedResult = store.paused(manifest.route.canonical());
    REQUIRE(pausedResult);
    CHECK(*pausedResult);

    REQUIRE(store.reset(manifest.route.canonical()));
    pausedResult = store.paused(manifest.route.canonical());
    REQUIRE(pausedResult);
    CHECK_FALSE(*pausedResult);

    auto duplicate = store.record("phase-0", ReviewVerdict::Modify, "conflicting outcome");
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().message.find("terminal") != std::string::npos);
  }

  TEST_CASE("Fleet process runner - timeout terminates the process group", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    auto request = ProcessRequest{
      .argv = {"sh", "-c", "trap '' TERM; sleep 30 & wait"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::milliseconds{100},
      .terminationGrace = std::chrono::milliseconds{100},
    };

    auto result = runner.run(request);

    CHECK(result.status == ProcessStatus::TimedOut);
    CHECK(result.elapsed < std::chrono::seconds{5});
  }

  TEST_CASE("Fleet process runner - environment is allowlisted", "[fleet][integration][process]")
  {
    REQUIRE(::setenv("AOBUS_FLEET_SECRET", "must-not-leak", 1) == 0);
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "test -z \"${AOBUS_FLEET_SECRET:-}\""},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGrace = std::chrono::seconds{1},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    REQUIRE(::unsetenv("AOBUS_FLEET_SECRET") == 0);
  }

  TEST_CASE("Fleet process runner - launch and signal outcomes are typed", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};

    auto missing = runner.run(ProcessRequest{
      .argv = {"aobus-fleet-command-that-does-not-exist"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{1},
      .terminationGrace = std::chrono::seconds{1},
    });
    CHECK(missing.status == ProcessStatus::LaunchFailed);

    auto signaled = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "kill -TERM $$"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGrace = std::chrono::seconds{1},
    });
    CHECK(signaled.status == ProcessStatus::Signaled);
    CHECK(signaled.signal == 15);
  }

  TEST_CASE("Fleet runner - mock proposal advisory and full council traces", "[fleet][integration][engine]")
  {
    auto temp = TempDirectory{};
    auto const repo = temp.path() / "repo";
    auto const out = temp.path() / "artifacts";
    std::filesystem::create_directories(repo);
    temp.write("repo/source.txt", "original\n");
    auto const buildScript =
      temp.write("repo/build.sh", "#!/bin/sh\ntest ! -e .git/poison && grep -Eq '^(original|updated)$' source.txt\n");
    std::filesystem::permissions(buildScript,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write | std::filesystem::perms::group_exec |
                                   std::filesystem::perms::group_read | std::filesystem::perms::others_exec |
                                   std::filesystem::perms::others_read);

    auto process = BoostProcessRunner{};
    auto init = process.run(ProcessRequest{
      .argv = {"git", "init", repo.string()},
      .cwd = temp.path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{10},
      .terminationGrace = std::chrono::seconds{1},
    });
    REQUIRE(init.status == ProcessStatus::Exited);
    REQUIRE(init.exitCode == 0);

    auto registry = Registry{};
    registry.authorities.emplace("write",
                                 AuthorityPolicy{.id = "write",
                                                 .filesystem = FilesystemAuthority::WritableCopy,
                                                 .network = NetworkAuthority::Off,
                                                 .contextView = ContextView::Full});
    registry.authorities.emplace("read",
                                 AuthorityPolicy{.id = "read",
                                                 .filesystem = FilesystemAuthority::ReadOnly,
                                                 .network = NetworkAuthority::Off,
                                                 .contextView = ContextView::Full});
    registry.agents.emplace(
      "editor",
      AgentDefinition{.id = "editor",
                      .vendor = "mock",
                      .model = "editor-v1",
                      .argvTemplate = {"sh", "-c", "printf 'updated\\n' > source.txt; printf poison > .git/poison"},
                      .promptDelivery = PromptDelivery::Stdin,
                      .environmentWhitelist = {"PATH"},
                      .credentialMounts = {},
                      .timeout = std::chrono::seconds{10},
                      .rateLimitKey = "mock-editor",
                      .defaultAuthority = "write"});
    registry.agents.emplace("no-op",
                            AgentDefinition{.id = "no-op",
                                            .vendor = "mock",
                                            .model = "no-op-v1",
                                            .argvTemplate = {"true"},
                                            .promptDelivery = PromptDelivery::Stdin,
                                            .environmentWhitelist = {"PATH"},
                                            .credentialMounts = {},
                                            .timeout = std::chrono::seconds{10},
                                            .rateLimitKey = "mock-no-op",
                                            .defaultAuthority = "write"});

    for (auto const& id : {"member-a", "member-b", "member-c"})
    {
      registry.agents.emplace(id,
                              AgentDefinition{.id = id,
                                              .vendor = "mock",
                                              .model = std::string{id} + "-v1",
                                              .argvTemplate = {"sh", "-c", std::format("printf '{} analysis\\n'", id)},
                                              .promptDelivery = PromptDelivery::Stdin,
                                              .environmentWhitelist = {"PATH"},
                                              .credentialMounts = {},
                                              .timeout = std::chrono::seconds{10},
                                              .rateLimitKey = id,
                                              .defaultAuthority = "read"});
    }

    registry.oracles.emplace("mock-build",
                             OracleDefinition{.id = "mock-build",
                                              .runner = OracleRunner::BuildDebug,
                                              .arguments = {},
                                              .property = "source contains the expected update",
                                              .prerequisites = {},
                                              .knownGaps = {},
                                              .baselinePolicy = BaselinePolicy::RequireGreen,
                                              .rulerPaths = {"build.sh"}});
    registry.bindings.emplace("proposal",
                              Binding{.taskKind = "proposal",
                                      .agent = "editor",
                                      .engine = EngineKind::Gate,
                                      .optOracle = "mock-build",
                                      .optRiskOracle = std::nullopt,
                                      .authority = "write",
                                      .gate = {.fanout = 2, .topK = 1, .maxRounds = 1, .churnLines = 20},
                                      .synthesis = {}});
    registry.bindings.emplace("advisory",
                              Binding{.taskKind = "advisory",
                                      .agent = "editor",
                                      .engine = EngineKind::Gate,
                                      .optOracle = std::nullopt,
                                      .optRiskOracle = std::nullopt,
                                      .authority = "write",
                                      .gate = {.fanout = 1, .topK = 1, .maxRounds = 1, .churnLines = 20},
                                      .synthesis = {}});
    registry.bindings.emplace(
      "council",
      Binding{.taskKind = "council",
              .agent = "member-a",
              .engine = EngineKind::Synthesis,
              .optOracle = std::nullopt,
              .optRiskOracle = std::nullopt,
              .authority = "read",
              .gate = {},
              .synthesis = {.roster = {"member-a", "member-b", "member-c"}, .depth = CouncilDepth::Full, .quorum = 2}});
    registry.bindings.emplace("fallback",
                              Binding{.taskKind = "fallback",
                                      .agent = "no-op",
                                      .engine = EngineKind::Gate,
                                      .optOracle = std::nullopt,
                                      .optRiskOracle = std::nullopt,
                                      .authority = "write",
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
                         .invariant = "Preserve the real repository while producing the requested artifact.",
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
    auto const proposal = std::ranges::find(result->manifests, std::string{"proposal-phase"}, &ReviewManifest::phaseId);
    auto const advisory = std::ranges::find(result->manifests, std::string{"advisory-phase"}, &ReviewManifest::phaseId);
    auto const council = std::ranges::find(result->manifests, std::string{"council-phase"}, &ReviewManifest::phaseId);
    auto const fallback = std::ranges::find(result->manifests, std::string{"fallback-phase"}, &ReviewManifest::phaseId);
    REQUIRE(proposal != result->manifests.end());
    REQUIRE(advisory != result->manifests.end());
    REQUIRE(council != result->manifests.end());
    REQUIRE(fallback != result->manifests.end());
    CHECK(proposal->mode == OutputMode::Proposal);
    CHECK(advisory->mode == OutputMode::Advisory);
    CHECK(council->mode == OutputMode::Advisory);
    CHECK(fallback->mode == OutputMode::Advisory);
    CHECK(fallback->route.agentId == "editor");
    CHECK(std::filesystem::exists(out / "proposal-phase" / "patch"));
    CHECK(std::filesystem::exists(out / "advisory-phase" / "review.md"));
    CHECK(std::filesystem::exists(out / "council-phase" / "dossier.md"));
    CHECK(std::filesystem::exists(out / "proposal-phase" / "trace.yaml"));
    CHECK(std::filesystem::exists(out / "audit.yaml"));
    auto realSource = std::ifstream{repo / "source.txt"};
    auto realValue = std::string{};
    std::getline(realSource, realValue);
    CHECK(realValue == "original");
    auto hasJsonArtifact = false;

    for (auto const& entry : std::filesystem::recursive_directory_iterator{out})
    {
      hasJsonArtifact = hasJsonArtifact || entry.path().extension() == ".json" || entry.path().extension() == ".jsonl";
    }

    CHECK_FALSE(hasJsonArtifact);
  }
} // namespace ao::fleet::test
