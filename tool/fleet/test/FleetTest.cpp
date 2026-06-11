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
#include <cstddef>
#include <cstdlib>
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

  TEST_CASE("Fleet registry - production registry validates and resolves", "[fleet][unit][yaml]")
  {
    auto registry = loadRegistry(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");

    REQUIRE(registry);
    CHECK(registry->agents.contains("dspro"));
    CHECK(registry->oracles.contains("test-all"));
    CHECK(registry->bindings.at("implement-plan").engine == EngineKind::Gate);
    CHECK(registry->bindings.at("council-plan").engine == EngineKind::Synthesis);
    auto councilIntent = PhaseIntent{.id = "council-check",
                                     .taskKind = "council-review",
                                     .invariant = "Identify correctness and regression risks.",
                                     .scope = {},
                                     .dependsOn = {},
                                     .overrides = {},
                                     .body = "Review the change."};
    auto resolvedCouncil = resolvePhase(*registry, councilIntent);
    REQUIRE(resolvedCouncil);
    CHECK(resolvedCouncil->agent.id == "claude-opus");
    CHECK_FALSE(registry->rulerPaths.empty());
    CHECK(registry->oracles.at("test-asan").runner == OracleRunner::TestAsan);
    CHECK(registry->oracles.at("test-tsan").runner == OracleRunner::TestTsan);
    CHECK(registry->oracles.at("test-asan").optTimeout == std::chrono::milliseconds{3600000});
    CHECK_FALSE(registry->oracles.at("test-all").optTimeout.has_value());
    CHECK(registry->oracles.at("test-delta").arguments.contains("test-paths"));
    CHECK(registry->oracles.at("public-signature-delta").arguments.contains("header-prefixes"));
    CHECK_FALSE(registry->bindings.at("fix-lint").optRiskOracle.has_value());
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
      source += "extra: &anchor value\n";
      auto result = loadIntent(temp.write("anchor.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("anchors") != std::string::npos);
    }

    SECTION("aliases are rejected")
    {
      auto source = intentYaml();
      source += "extra: *anchor\n";
      auto result = loadIntent(temp.write("alias.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("aliases") != std::string::npos);
    }

    SECTION("tags are rejected")
    {
      auto source = intentYaml();
      source += "extra: !!str value\n";
      auto result = loadIntent(temp.write("tag.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("tags") != std::string::npos);
    }

    SECTION("merge keys are rejected")
    {
      auto source = intentYaml();
      source += "<<: {extra: value}\n";
      auto result = loadIntent(temp.write("merge.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("merge keys") != std::string::npos);
    }

    SECTION("yaml-like characters inside scalar content are accepted")
    {
      auto source = intentYaml();
      source += "  Run *Test.cpp suites, keep !queue.empty() checks, pass &config by reference.\n";
      auto result = loadIntent(temp.write("specials.yaml", source));
      REQUIRE(result);
      CHECK(result->body.find("*Test.cpp") != std::string::npos);

      auto again = loadIntent(temp.write("specials-round-trip.yaml", emitIntent(*result)));
      REQUIRE(again);
      CHECK(again->body == result->body);
    }

    SECTION("document size is bounded")
    {
      auto result = loadIntent(temp.write("large.yaml", std::string((2 * 1024 * 1024) + 1, 'x')));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("2 MiB") != std::string::npos);
    }
  }

  TEST_CASE("Fleet model - enum name tables drive toString and parsing", "[fleet][unit][model]")
  {
    CHECK(parseReviewVerdict("accept") == ReviewVerdict::Accept);
    CHECK(parseReviewVerdict("modify") == ReviewVerdict::Modify);
    CHECK(parseReviewVerdict("reject") == ReviewVerdict::Reject);
    CHECK_FALSE(parseReviewVerdict("approve").has_value());

    for (auto const& [value, name] : kFailureReasonNames)
    {
      CHECK(toString(value) == name);
    }

    for (auto const& [value, name] : kOracleRunnerNames)
    {
      CHECK(toString(value) == name);
    }

    for (auto const& [value, name] : kEscalationActionNames)
    {
      CHECK(toString(value) == name);
    }
  }

  TEST_CASE("Fleet intent - full override set round trips", "[fleet][unit][yaml]")
  {
    auto temp = TempDirectory{};
    auto intent = loadIntent(temp.write("base.yaml", intentYaml()));
    REQUIRE(intent);

    intent->overrides = IntentOverrides{.optAgent = "dspro",
                                        .optEngine = EngineKind::Synthesis,
                                        .optOracle = "test-core",
                                        .optRiskOracle = "test-delta",
                                        .optFanout = 2,
                                        .optTopK = 1,
                                        .optMaxRounds = 3,
                                        .optChurnLines = 100,
                                        .optDepth = CouncilDepth::Panel,
                                        .optQuorum = 2};

    auto again = loadIntent(temp.write("round.yaml", emitIntent(*intent)));
    REQUIRE(again);
    CHECK(again->overrides == intent->overrides);
  }

  TEST_CASE("Fleet YAML stream - control bytes in scalars round trip", "[fleet][unit][yaml]")
  {
    auto temp = TempDirectory{};
    auto const path = temp.path() / "stream.yaml";
    auto const hostile = std::string{"esc\x1b[31m bell\x07 del\x7f raw\xff end"};

    auto document = std::string{"schema: aobus-fleet-trace-event/v1\nvalue: "};
    document += yamlScalar(hostile);
    document += "\n";
    REQUIRE(appendYamlDocument(path, document));

    auto stream = readScalarStream(path, "aobus-fleet-trace-event/v1");
    REQUIRE(stream);
    CHECK_FALSE(stream->trailingCorruption);
    REQUIRE(stream->documents.size() == 1);
    CHECK(stream->documents.front().at("value") == hostile);
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

  TEST_CASE("Fleet scheduler - dependency graph validation and cycle rejection", "[fleet][unit][scheduler]")
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

    SECTION("accepts a DAG")
    {
      CHECK(Scheduler::validate({third, first, second}));
    }

    SECTION("rejects a cycle")
    {
      first.dependsOn = {"c"};
      auto result = Scheduler::validate({first, second, third});
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("cycle") != std::string::npos);
    }
  }

  TEST_CASE("Fleet patch guard - enforces scope operation churn and rulers", "[fleet][unit][guard]")
  {
    auto patch = PatchArtifact{
      .candidateId = "candidate-a",
      .patch = "diff --git a/lib/audio/Player.cpp b/lib/audio/Player.cpp\n",
      .touchedFiles = {TouchedFile{.path = "lib/audio/Player.cpp", .operation = ScopeOperation::Modify}},
      .addedLines = 3,
      .removedLines = 2,
    };
    auto const scope = std::vector{ScopeRule{.path = "lib/audio/Player.cpp", .operations = {ScopeOperation::Modify}}};

    CHECK(PatchGuard::inspect(patch, scope, 10, {"tool/fleet"}).accepted);

    patch.touchedFiles = {TouchedFile{.path = "lib/audio/Other.cpp", .operation = ScopeOperation::Modify}};
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);

    patch.touchedFiles = {TouchedFile{.path = "lib/audio/Player.cpp", .operation = ScopeOperation::Modify}};
    patch.addedLines = 20;
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ChurnExceeded);

    patch.addedLines = 3;
    patch.touchedFiles = {TouchedFile{.path = "tool/fleet/src/Engine.cpp", .operation = ScopeOperation::Modify}};
    CHECK(
      PatchGuard::inspect(patch, {ScopeRule{"tool/fleet/src/Engine.cpp", {ScopeOperation::Modify}}}, 10, {"tool/fleet"})
        .failure == FailureReason::ScopeViolation);

    // A scope rule cannot authorize an operation the diff status does not allow.
    patch.touchedFiles = {TouchedFile{.path = "lib/audio/Player.cpp", .operation = ScopeOperation::Delete}};
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);

    // Nested CMakeLists.txt files are ruler-protected by basename at any depth.
    patch.touchedFiles = {TouchedFile{.path = "lib/audio/CMakeLists.txt", .operation = ScopeOperation::Modify}};
    CHECK(
      PatchGuard::inspect(patch, {ScopeRule{"lib/audio/CMakeLists.txt", {ScopeOperation::Modify}}}, 10, {}).failure ==
      FailureReason::ScopeViolation);

    // Forbidden markers only count at the start of a line; added source lines that merely
    // contain the words must pass.
    patch.touchedFiles = {TouchedFile{.path = "lib/audio/Player.cpp", .operation = ScopeOperation::Modify}};
    patch.patch = "diff --git a/lib/audio/Player.cpp b/lib/audio/Player.cpp\n"
                  "+  // documentation: rename from X, copy from Y, old mode bits\n";
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).accepted);

    patch.patch += "old mode 100644\nnew mode 100755\n";
    CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);

    patch.patch = "diff --git a/x b/x\nrename from lib/audio/Player.cpp\n";
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
                        .scopeRiskClass = "private"},
      .summary = "test",
      .optEscalationAction = std::nullopt,
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

  TEST_CASE("Fleet process runner - children get default SIGPIPE disposition", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    // The runner ignores SIGPIPE in its own process; the launcher hook must
    // hand SIG_DFL back to the child or shell pipelines inside spawned agents
    // would silently change behavior. SIGPIPE is signal 13, bit 12 (4096) in
    // the SigIgn mask of /proc/self/status.
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "mask=$(grep SigIgn /proc/self/status | cut -f2); test $(( 0x$mask & 4096 )) -eq 0"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGrace = std::chrono::seconds{1},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
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

  TEST_CASE("Fleet process runner - orphaned pipe holders do not hang the runner", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    // The backgrounded sleep inherits the stdio pipes and outlives the shell, so the runner
    // only returns if it force-closes the pipes after the child exits.
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "sleep 30 & exit 7"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{20},
      .terminationGrace = std::chrono::milliseconds{200},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 7);
    CHECK(result.elapsed < std::chrono::seconds{10});
  }

  TEST_CASE("Fleet process runner - unread standard input neither kills nor hangs the runner",
            "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    // Larger than the kernel pipe buffer, so the write is still pending when the child exits.
    auto input = std::string(static_cast<std::size_t>(1) << 20, 'x');
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "exit 0"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = std::move(input),
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{20},
      .terminationGrace = std::chrono::seconds{2},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    CHECK(result.elapsed < std::chrono::seconds{10});
  }

  namespace
  {
    struct RecordingRunner final : IProcessRunner
    {
      std::vector<ProcessRequest> requests;
      ProcessResult result;

      RecordingRunner()
      {
        result.status = ProcessStatus::Exited;
        result.exitCode = 0;
      }

      ProcessResult run(ProcessRequest const& request) override
      {
        requests.push_back(request);
        return result;
      }
    };

    bool containsSequence(std::vector<std::string> const& argv, std::vector<std::string> const& sequence)
    {
      return std::ranges::search(argv, sequence).begin() != argv.end();
    }
  } // namespace

  TEST_CASE("Fleet namespace runner - sandbox mounts shape the bwrap argv", "[fleet][unit][substrate]")
  {
    auto recorder = RecordingRunner{};
    auto runner = NamespaceRunner{recorder};
    auto const realRepo = std::filesystem::path{"/repo/real"};
    auto const workspace = std::filesystem::path{"/work/copy"};
    auto const* home = std::getenv("HOME");
    REQUIRE(home != nullptr);

    SECTION("agent mounts bind HOME before the workspace")
    {
      auto request = ProcessRequest{};
      request.argv = {"agent-cli"};
      [[maybe_unused]] auto const ignored =
        runner.run(realRepo, workspace, SandboxMounts{.writableBinds = {}, .bindHome = true}, std::move(request));
      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      CHECK(containsSequence(argv, {"--bind", home, home}));
      auto const homeBind = std::ranges::search(argv, std::vector<std::string>{"--bind", home, home}).begin();
      auto const workspaceBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", workspace.string(), realRepo.string()}).begin();
      CHECK(homeBind < workspaceBind);
      CHECK_FALSE(std::ranges::contains(argv, std::string{"--unshare-net"}));
    }

    SECTION("oracle mounts add writable binds after the workspace and never bind HOME")
    {
      auto mounts = SandboxMounts{.writableBinds = {{"/host/oracle-build", "/tmp/build"}}, .bindHome = false};
      auto request = ProcessRequest{};
      request.argv = {"./build.sh", "debug"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, mounts, std::move(request));
      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      CHECK_FALSE(containsSequence(argv, {"--bind", home, home}));
      auto const workspaceBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", workspace.string(), realRepo.string()}).begin();
      auto const buildBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", "/host/oracle-build", "/tmp/build"}).begin();
      CHECK(workspaceBind < buildBind);
    }
  }

  TEST_CASE("Fleet snapshot provider - stale base destination is replaced", "[fleet][integration][substrate]")
  {
    auto temp = TempDirectory{};
    auto const repo = temp.path() / "repo";
    std::filesystem::create_directories(repo);
    temp.write("repo/source.txt", "fresh\n");
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
    REQUIRE(init.exitCode == 0);

    auto const destination = temp.path() / "out" / ".base";
    temp.write("out/.base/stale.txt", "left over by a crashed run\n");

    auto snapshot = SnapshotProvider{process};
    auto base = snapshot.createImmutableBase(repo, destination);

    REQUIRE(base);
    CHECK_FALSE(std::filesystem::exists(destination / "stale.txt"));
    CHECK(std::filesystem::exists(destination / "source.txt"));
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
    registry.agents.emplace(
      "editor",
      AgentDefinition{.id = "editor",
                      .model = "editor-v1",
                      .argvTemplate = {"sh", "-c", "printf 'updated\\n' > source.txt; printf poison > .git/poison"},
                      .promptDelivery = PromptDelivery::Stdin,
                      .environmentWhitelist = {"PATH"},
                      .timeout = std::chrono::seconds{10},
                      .rateLimitKey = "mock-editor"});
    registry.agents.emplace("no-op",
                            AgentDefinition{.id = "no-op",
                                            .model = "no-op-v1",
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
                        .model = std::string{id} + "-v1",
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
                                            .model = "member-fail-v1",
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
                                              .rulerPaths = {"build.sh"},
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
    // The route key carries the concrete oracle version everywhere, never a placeholder.
    CHECK(proposal->route.oracleVersion.size() == 16);
    auto const proposalManifest = readFile(out / "proposal-phase" / "manifest.yaml");
    CHECK(proposalManifest.find("schema: aobus-fleet-manifest/v1") != std::string::npos);
    CHECK(proposalManifest.find("resolved-at-run") == std::string::npos);
    CHECK(proposalManifest.find("escalation-action:") != std::string::npos);
    CHECK(proposalManifest.find("authority") == std::string::npos);
    auto const resolvedPhase = readFile(out / "proposal-phase" / "resolved.yaml");
    CHECK(resolvedPhase.find("schema: aobus-fleet-resolved/v1") != std::string::npos);
    CHECK(resolvedPhase.find("authority:") == std::string::npos);
    CHECK(std::filesystem::exists(out / "proposal-phase" / "patch"));
    CHECK(std::filesystem::exists(out / "advisory-phase" / "review.md"));
    CHECK(std::filesystem::exists(out / "council-phase" / "dossier.md"));
    CHECK_FALSE(std::filesystem::exists(out / "council-phase" / "members" / "member-a" / "r1.log"));
    auto const memberPrompt = readFile(out / "council-phase" / "members" / "member-a" / "r1" / "prompt.md");
    CHECK(memberPrompt.find("Council round: 1 of 3 (independent draft)") != std::string::npos);
    CHECK(memberPrompt.find("Scope (focus on these paths and operations):\n- source.txt: modify") != std::string::npos);
    CHECK(memberPrompt.find("you will then revise it") != std::string::npos);
    CHECK(memberPrompt.find("real repository") == std::string::npos);
    CHECK(memberPrompt.find("workspace is disposable") == std::string::npos);
    CHECK(memberPrompt.find("repository copy") == std::string::npos);
    CHECK(readFile(out / "council-phase" / "members" / "member-a" / "r1" / "stdout.txt") == "member-a draft\n");
    CHECK(readFile(out / "council-phase" / "members" / "member-a" / "r1" / "stderr.txt") == "member-a stderr\n");
    auto const challengePrompt = readFile(out / "council-phase" / "members" / "member-a" / "r2" / "prompt.md");
    CHECK(challengePrompt.find("Council round: 2 of 3 (cross-challenge)") != std::string::npos);
    CHECK(challengePrompt.find("verify their claims against the repository") != std::string::npos);
    CHECK(challengePrompt.find("given to each draft's author for revision") != std::string::npos);
    CHECK(challengePrompt.find("member-a draft") == std::string::npos);
    CHECK(challengePrompt.find("member-b draft") != std::string::npos);
    CHECK(challengePrompt.find("member-c draft") != std::string::npos);
    auto const revisionPrompt = readFile(out / "council-phase" / "members" / "member-a" / "r3" / "prompt.md");
    CHECK(revisionPrompt.find("Council round: 3 of 3 (revision)") != std::string::npos);
    CHECK(revisionPrompt.find("Your prior draft:\nmember-a draft") != std::string::npos);
    CHECK(revisionPrompt.find("Your own challenge notes from the previous round:\nmember-a challenge") !=
          std::string::npos);
    CHECK(revisionPrompt.find("--- member-a ---") == std::string::npos);
    CHECK(revisionPrompt.find("member-b challenge") != std::string::npos);
    CHECK(revisionPrompt.find("member-c challenge") != std::string::npos);
    auto const memberResult = readFile(out / "council-phase" / "members" / "member-a" / "r1" / "result.yaml");
    CHECK(memberResult.find("schema: aobus-fleet-member-run/v1") != std::string::npos);
    CHECK(memberResult.find("authority:") == std::string::npos);
    CHECK(memberResult.find("quarantined: false") != std::string::npos);
    auto const failedResult = readFile(out / "council-phase" / "members" / "member-fail" / "r1" / "result.yaml");
    CHECK(readFile(out / "council-phase" / "members" / "member-fail" / "r1" / "stderr.txt") == "member-fail stderr\n");
    CHECK(failedResult.find("exit-code: 9") != std::string::npos);
    CHECK(failedResult.find("quarantine-reason: \"non-zero-exit\"") != std::string::npos);
    CHECK(std::filesystem::exists(out / "proposal-phase" / "trace.yaml"));
    CHECK(std::filesystem::exists(out / "audit.yaml"));
    auto realSource = std::ifstream{repo / "source.txt"};
    auto realValue = std::string{};
    std::getline(realSource, realValue);
    CHECK(realValue == "original");
    CHECK_FALSE(std::filesystem::exists(repo / ".codex"));
    auto hasJsonArtifact = false;

    for (auto const& entry : std::filesystem::recursive_directory_iterator{out})
    {
      hasJsonArtifact = hasJsonArtifact || entry.path().extension() == ".json" || entry.path().extension() == ".jsonl";
    }

    CHECK_FALSE(hasJsonArtifact);
  }

  namespace
  {
    void runCommand(IProcessRunner& process, std::filesystem::path const& cwd, std::vector<std::string> argv)
    {
      auto result = process.run(ProcessRequest{
        .argv = std::move(argv),
        .cwd = cwd,
        .standardInput = {},
        .environmentWhitelist = {"PATH"},
        .environment = {},
        .timeout = std::chrono::seconds{10},
        .terminationGrace = std::chrono::seconds{1},
      });
      REQUIRE(result.status == ProcessStatus::Exited);
      REQUIRE(result.exitCode == 0);
    }

    void initGitRepo(IProcessRunner& process, std::filesystem::path const& repo, std::filesystem::path const& cwd)
    {
      runCommand(process, cwd, {"git", "init", repo.string()});
    }

    Registry schedulerRegistry()
    {
      return Registry{};
    }

    AgentDefinition shellAgent(std::string id, std::string script)
    {
      auto model = id + "-v1";
      return AgentDefinition{.id = std::move(id),
                             .model = std::move(model),
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
    auto temp = TempDirectory{};
    auto const repo = temp.path() / "repo";
    auto const out = temp.path() / "artifacts";
    std::filesystem::create_directories(repo);
    temp.write("repo/source.txt", "original\n");
    auto process = BoostProcessRunner{};
    initGitRepo(process, repo, temp.path());

    auto registry = schedulerRegistry();
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
    auto temp = TempDirectory{};
    auto const repo = temp.path() / "repo";
    auto const out = temp.path() / "artifacts";
    std::filesystem::create_directories(repo);
    temp.write("repo/source.txt", "original\n");
    auto process = BoostProcessRunner{};
    initGitRepo(process, repo, temp.path());

    auto registry = schedulerRegistry();
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

  TEST_CASE("Fleet patch extractor - status letters map to scope operations", "[fleet][integration][substrate]")
  {
    auto temp = TempDirectory{};
    auto const repo = temp.path() / "repo";
    std::filesystem::create_directories(repo);
    temp.write("repo/keep.txt", "one\n");
    temp.write("repo/gone.txt", "two\n");
    auto process = BoostProcessRunner{};
    initGitRepo(process, repo, temp.path());
    runCommand(process, repo, {"git", "-C", repo.string(), "add", "-A"});
    runCommand(process,
               repo,
               {"git",
                "-C",
                repo.string(),
                "-c",
                "user.name=Fleet Test",
                "-c",
                "user.email=fleet@test",
                "commit",
                "-m",
                "seed"});

    temp.write("repo/keep.txt", "one changed\n");
    std::filesystem::remove(repo / "gone.txt");
    // A created path with spaces and non-ASCII bytes exercises the NUL-delimited parsing.
    temp.write("repo/sub/añadido nuevo.txt", "three\n");

    auto extractor = PatchExtractor{process};
    auto patch = extractor.extract(repo, "candidate-z");

    REQUIRE(patch);
    REQUIRE(patch->touchedFiles.size() == 3);
    auto operationOf = [&](std::string_view path)
    {
      auto found = std::ranges::find(patch->touchedFiles, std::filesystem::path{path}, &TouchedFile::path);
      REQUIRE(found != patch->touchedFiles.end());
      return found->operation;
    };
    CHECK(operationOf("keep.txt") == ScopeOperation::Modify);
    CHECK(operationOf("gone.txt") == ScopeOperation::Delete);
    CHECK(operationOf("sub/añadido nuevo.txt") == ScopeOperation::Create);
  }
} // namespace ao::fleet::test
