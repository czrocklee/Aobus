// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/CouncilSchema.h"
#include "council/Serialization.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace ao::council::test
{
  namespace
  {
    std::filesystem::path productionRegistry()
    {
      return std::filesystem::path{AOBUS_SOURCE_DIR} / "config" / "agent-council.yaml";
    }
  } // namespace

  TEST_CASE("Registry - production registry is council-only", "[council][unit][yaml]")
  {
    auto registry = loadRegistry(productionRegistry());
    REQUIRE(registry);
    CHECK(registry->harnesses.contains("codex"));
    CHECK(registry->agents.contains("anthropic-opus"));
    CHECK(registry->councils.contains("council-plan"));
    CHECK(registry->councils.contains("council-review"));

    auto const& review = registry->councils.at("council-review");
    CHECK(review.parameters.depth == Depth::Challenge);
    CHECK(review.parameters.quorum == 3);
    CHECK(review.parameters.roster ==
          std::vector<std::string>{"anthropic-opus", "openai-gpt", "google-gemini-pro", "deepseek-pro"});
  }

  TEST_CASE("Registry - resolves intent overrides", "[council][unit][yaml]")
  {
    auto registry = loadRegistry(productionRegistry());
    REQUIRE(registry);
    auto intent =
      PhaseIntent{.id = "phase-a",
                  .taskKind = "council-review",
                  .invariant = "Preserve behavior.",
                  .overrides = IntentOverrides{.optRoster = std::vector<std::string>{"anthropic-opus", "openai-gpt"},
                                               .optDepth = Depth::Panel,
                                               .optQuorum = 2},
                  .body = "Review the change."};

    auto resolved = resolvePhase(*registry, intent);
    REQUIRE(resolved);
    CHECK(resolved->definition.parameters.roster == std::vector<std::string>{"anthropic-opus", "openai-gpt"});
    CHECK(resolved->definition.parameters.depth == Depth::Panel);
    CHECK(resolved->definition.parameters.quorum == 2);
  }

  TEST_CASE("Registry - rejects unknown task kind during resolution", "[council][unit][yaml]")
  {
    auto registry = loadRegistry(productionRegistry());
    REQUIRE(registry);
    auto intent = PhaseIntent{
      .id = "phase-a", .taskKind = "missing-council", .invariant = "Preserve behavior.", .body = "Review the change."};

    auto resolved = resolvePhase(*registry, intent);
    REQUIRE_FALSE(resolved);
    CHECK(resolved.error().message.contains("no definition for task-kind 'missing-council'"));
  }

  TEST_CASE("Registry - rejects invalid intent overrides", "[council][unit][yaml]")
  {
    auto registry = loadRegistry(productionRegistry());
    REQUIRE(registry);
    auto intent = PhaseIntent{
      .id = "phase-a", .taskKind = "council-review", .invariant = "Preserve behavior.", .body = "Review the change."};

    SECTION("unknown agent")
    {
      intent.overrides.optRoster = std::vector<std::string>{"anthropic-opus", "missing-agent"};
      intent.overrides.optQuorum = 2;
      auto resolved = resolvePhase(*registry, intent);
      REQUIRE_FALSE(resolved);
      CHECK(resolved.error().message.contains("unknown agent"));
    }

    SECTION("duplicate agent")
    {
      intent.overrides.optRoster = std::vector<std::string>{"anthropic-opus", "anthropic-opus"};
      intent.overrides.optQuorum = 2;
      auto resolved = resolvePhase(*registry, intent);
      REQUIRE_FALSE(resolved);
      CHECK(resolved.error().message.contains("repeats agent"));
    }

    SECTION("duplicate vendor")
    {
      intent.overrides.optRoster = std::vector<std::string>{"anthropic-opus", "anthropic-sonnet"};
      intent.overrides.optQuorum = 2;
      auto resolved = resolvePhase(*registry, intent);
      REQUIRE_FALSE(resolved);
      CHECK(resolved.error().message.contains("repeats vendor"));
    }

    SECTION("quorum outside roster")
    {
      intent.overrides.optRoster = std::vector<std::string>{"anthropic-opus", "openai-gpt"};
      intent.overrides.optQuorum = 3;
      auto resolved = resolvePhase(*registry, intent);
      REQUIRE_FALSE(resolved);
      CHECK(resolved.error().message.contains("quorum must be between"));
    }
  }

  TEST_CASE("Registry - rejects unknown schema", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "registry.yaml",
                                R"(schema: aobus-delegation-registry/v1
harnesses: {}
agents: {}
councils: {}
)");

    auto registry = loadRegistry(path);
    REQUIRE_FALSE(registry);
    CHECK(registry.error().message.contains("invalid registry schema"));
  }

  TEST_CASE("Registry - rejects path-reserved agent ids", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "registry.yaml",
                                R"(schema: aobus-council-registry/v1
harnesses:
  shell:
    argv: [sh, -c, "cat"]
agents:
  ..:
    harness: shell
    model: model-a
    vendor: vendor-a
councils:
  council-review:
    roster: [".."]
    quorum: 1
)");

    auto registry = loadRegistry(path);
    REQUIRE_FALSE(registry);
    CHECK(registry.error().message.contains("reserved"));
  }

  TEST_CASE("Registry - roster members must be independent", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "registry.yaml",
                                R"(schema: aobus-council-registry/v1
harnesses:
  shell:
    argv: [sh, -c, "cat"]
    prompt-delivery: stdin
    environment-whitelist: [PATH]
    timeout-ms: 1000
agents:
  agent-a:
    harness: shell
    model: model-a
    vendor: same
  agent-b:
    harness: shell
    model: model-b
    vendor: same
councils:
  council-review:
    roster: [agent-a, agent-b]
    depth: panel
    quorum: 2
)");

    auto registry = loadRegistry(path);
    REQUIRE_FALSE(registry);
    CHECK(registry.error().message.contains("repeats vendor"));
  }

  TEST_CASE("Registry - prompt placeholders survive agent materialization", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "registry.yaml",
                                R"(schema: aobus-council-registry/v1
harnesses:
  shell:
    argv: [agent-cli, --prompt-file, "{prompt-file}", --model, "{model}"]
    prompt-delivery: file
    environment-whitelist: [PATH]
    timeout-ms: 1000
agents:
  agent-a:
    harness: shell
    model: model-a
    vendor: vendor-a
councils:
  council-review:
    roster: [agent-a]
    depth: panel
    quorum: 1
)");

    auto registry = loadRegistry(path);
    REQUIRE(registry);
    CHECK(registry->agents.at("agent-a").argvTemplate ==
          std::vector<std::string>{"agent-cli", "--prompt-file", "{prompt-file}", "--model", "model-a"});
  }

  TEST_CASE("Registry - effort placeholders require an effort value", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "registry.yaml",
                                R"(schema: aobus-council-registry/v1
harnesses:
  shell:
    argv: [agent-cli, --effort, "{effort}"]
agents:
  agent-a:
    harness: shell
    model: model-a
    vendor: vendor-a
councils:
  council-review:
    roster: [agent-a]
    depth: panel
    quorum: 1
)");

    auto registry = loadRegistry(path);
    REQUIRE_FALSE(registry);
    CHECK(registry.error().message.contains("effort is empty"));
  }
} // namespace ao::council::test
