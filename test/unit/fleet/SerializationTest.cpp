// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Serialization.h"

#include "fleet/Model.h"
#include "test/fleet/FleetTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace ao::fleet::test
{
  TEST_CASE("Fleet review verdict - parser accepts only protocol values", "[fleet][unit][yaml]")
  {
    CHECK(parseReviewVerdict("accept") == ReviewVerdict::Accept);
    CHECK(parseReviewVerdict("modify") == ReviewVerdict::Modify);
    CHECK(parseReviewVerdict("reject") == ReviewVerdict::Reject);
    CHECK_FALSE(parseReviewVerdict("approve").has_value());
  }

  TEST_CASE("Fleet registry - production registry validates and resolves", "[fleet][unit][yaml]")
  {
    auto registry = loadRegistry(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");

    REQUIRE(registry);
    CHECK(registry->harnesses.contains("opencode"));
    CHECK(registry->agents.contains("deepseek-pro"));
    auto const& deepseek = registry->agents.at("deepseek-pro");
    CHECK(deepseek.harness == "opencode");
    CHECK(deepseek.vendor == "deepseek");
    CHECK(deepseek.rateLimitKey == "opencode-go");
    CHECK(deepseek.timeout == std::chrono::minutes{20});
    CHECK(std::ranges::contains(deepseek.argvTemplate, std::string_view{"opencode-go/deepseek-v4-pro"}));
    CHECK(deepseek.modelVersion() == "opencode-go/deepseek-v4-pro");
    auto const& opus = registry->agents.at("anthropic-opus");
    CHECK(opus.effort == "xhigh");
    CHECK(std::ranges::contains(opus.argvTemplate, std::string_view{"xhigh"}));
    CHECK(std::ranges::contains(opus.argvTemplate, std::string_view{"claude-opus-4-8"}));
    CHECK(opus.modelVersion() == "claude-opus-4-8@xhigh");
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
    CHECK(resolvedCouncil->agent.id == "anthropic-opus");
    CHECK_FALSE(registry->rulerPaths.empty());
    CHECK(registry->oracles.at("test-asan").runner == OracleRunner::TestAsan);
    CHECK(registry->oracles.at("test-tsan").runner == OracleRunner::TestTsan);
    CHECK(registry->oracles.at("test-asan").optTimeout == std::chrono::hours{1});
    CHECK_FALSE(registry->oracles.at("test-all").optTimeout.has_value());
    CHECK(registry->oracles.at("test-delta").arguments.contains("test-paths"));
    CHECK(registry->oracles.at("public-signature-delta").arguments.contains("header-prefixes"));
    CHECK_FALSE(registry->bindings.at("fix-lint").optRiskOracle.has_value());
  }

  TEST_CASE("Fleet registry - invalid references placeholders and search fail statically", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const production = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");

    SECTION("unknown agent reference")
    {
      auto source = production;
      auto const position = source.find("agent: deepseek-pro");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"agent: deepseek-pro"}.size(), "agent: missing");
      auto result = loadRegistry(writeFile(temp, "missing-agent.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown agent") != std::string::npos);
    }

    SECTION("unknown harness reference")
    {
      auto source = production;
      auto const position = source.find("harness: codex");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"harness: codex"}.size(), "harness: missing");
      auto result = loadRegistry(writeFile(temp, "missing-harness.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown harness") != std::string::npos);
    }

    SECTION("harness argv without the model placeholder")
    {
      auto source = production;
      auto const needle = std::string{", -m, \"{model}\"]"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "]");
      auto result = loadRegistry(writeFile(temp, "no-model.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("must use '{model}'") != std::string::npos);
    }

    SECTION("missing effort for an effort-driven harness")
    {
      auto source = production;
      auto const needle = std::string{"\n    effort: xhigh"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "");
      auto result = loadRegistry(writeFile(temp, "missing-effort.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("must set effort") != std::string::npos);
    }

    SECTION("effort on a harness that does not use it")
    {
      auto source = production;
      auto const needle = std::string{"vendor: google"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "vendor: google\n    effort: high");
      auto result = loadRegistry(writeFile(temp, "stray-effort.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("does not use '{effort}'") != std::string::npos);
    }

    SECTION("empty model")
    {
      auto source = production;
      auto const needle = std::string{"model: gpt-5.5"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "model: \"\"");
      auto result = loadRegistry(writeFile(temp, "empty-model.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("model must not be empty") != std::string::npos);
    }

    SECTION("placeholder braces inside a model id")
    {
      auto source = production;
      auto const needle = std::string{"model: gpt-5.5"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "model: \"gpt{prompt}\"");
      auto result = loadRegistry(writeFile(temp, "brace-model.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("placeholder braces") != std::string::npos);
    }

    SECTION("empty harness rate-limit-key")
    {
      auto source = production;
      auto const needle = std::string{"rate-limit-key: openai"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "rate-limit-key: \"\"");
      auto result = loadRegistry(writeFile(temp, "empty-rate-key.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("rate-limit-key must not be empty") != std::string::npos);
    }

    SECTION("unknown escalation route")
    {
      auto source = production;
      auto const needle = std::string{"route: openai-gpt"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "route: missing");
      auto result = loadRegistry(writeFile(temp, "missing-route.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown agent") != std::string::npos);
    }

    SECTION("duplicate roster member")
    {
      auto source = production;
      auto const needle = std::string{"roster: [anthropic-opus, openai-gpt, google-gemini-pro, deepseek-pro]"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(
        position, needle.size(), "roster: [anthropic-opus, anthropic-opus, google-gemini-pro, deepseek-pro]");
      auto result = loadRegistry(writeFile(temp, "duplicate-roster.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("twice") != std::string::npos);
    }

    SECTION("single-vendor council roster")
    {
      auto source = production;

      for (auto const* vendor : {"vendor: deepseek", "vendor: openai", "vendor: google"})
      {
        auto const position = source.find(vendor);
        REQUIRE(position != std::string::npos);
        source.replace(position, std::string_view{vendor}.size(), "vendor: anthropic");
      }

      auto result = loadRegistry(writeFile(temp, "single-vendor.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("cross-vendor") != std::string::npos);
    }

    SECTION("per-agent timeout override is honored")
    {
      auto source = production;
      auto const needle = std::string{"vendor: openai"};
      auto const position = source.find(needle);
      REQUIRE(position != std::string::npos);
      source.replace(position, needle.size(), "vendor: openai\n    timeout-ms: 60000");
      auto result = loadRegistry(writeFile(temp, "timeout-override.yaml", source));
      REQUIRE(result);
      CHECK(result->agents.at("openai-gpt").timeout == std::chrono::minutes{1});
      CHECK(result->agents.at("deepseek-pro").timeout == std::chrono::minutes{20});
    }

    SECTION("unknown argv placeholder")
    {
      auto source = production;
      auto const position = source.find("argv: [opencode");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"argv: [opencode"}.size(), "argv: [\"{bogus}\", opencode");
      auto result = loadRegistry(writeFile(temp, "placeholder.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown placeholder") != std::string::npos);
    }

    SECTION("search binding")
    {
      auto source = production;
      auto const position = source.find("engine: gate");
      REQUIRE(position != std::string::npos);
      source.replace(position, std::string{"engine: gate"}.size(), "engine: search");
      auto result = loadRegistry(writeFile(temp, "search.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unsupported search engine") != std::string::npos);
    }
  }

  TEST_CASE("Fleet intent - strict schema and round trip", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};

    SECTION("valid intent round trips")
    {
      auto first = loadIntent(writeFile(temp, "first.yaml", intentYaml()));
      REQUIRE(first);
      auto second = loadIntent(writeFile(temp, "second.yaml", emitIntent(*first)));
      REQUIRE(second);
      CHECK(second->id == first->id);
      CHECK(second->scope == first->scope);
      CHECK(second->body == first->body);
    }

    SECTION("unknown keys are rejected")
    {
      auto source = intentYaml();
      source += "surprise: true\n";
      auto result = loadIntent(writeFile(temp, "unknown.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unknown field") != std::string::npos);
    }

    SECTION("duplicate keys are rejected")
    {
      auto source = intentYaml();
      source += "body: duplicate\n";
      auto result = loadIntent(writeFile(temp, "duplicate.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("duplicate key") != std::string::npos);
    }

    SECTION("path traversal is rejected")
    {
      auto result = loadIntent(writeFile(temp, "traversal.yaml", intentYaml("phase-a", "", "../outside.cpp")));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("traversal") != std::string::npos);
    }

    SECTION("artifact ID traversal is rejected")
    {
      auto result = loadIntent(writeFile(temp, "id-traversal.yaml", intentYaml("../escape")));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("unsafe") != std::string::npos);
    }

    SECTION("anchors are rejected")
    {
      auto source = intentYaml();
      source += "extra: &anchor value\n";
      auto result = loadIntent(writeFile(temp, "anchor.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("anchors") != std::string::npos);
    }

    SECTION("aliases are rejected")
    {
      auto source = intentYaml();
      source += "extra: *anchor\n";
      auto result = loadIntent(writeFile(temp, "alias.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("aliases") != std::string::npos);
    }

    SECTION("tags are rejected")
    {
      auto source = intentYaml();
      source += "extra: !!str value\n";
      auto result = loadIntent(writeFile(temp, "tag.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("tags") != std::string::npos);
    }

    SECTION("merge keys are rejected")
    {
      auto source = intentYaml();
      source += "<<: {extra: value}\n";
      auto result = loadIntent(writeFile(temp, "merge.yaml", source));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("merge keys") != std::string::npos);
    }

    SECTION("yaml-like characters inside scalar content are accepted")
    {
      auto source = intentYaml();
      source += "  Run *Test.cpp suites, keep !queue.empty() checks, pass &config by reference.\n";
      auto result = loadIntent(writeFile(temp, "specials.yaml", source));
      REQUIRE(result);
      CHECK(result->body.find("*Test.cpp") != std::string::npos);

      auto again = loadIntent(writeFile(temp, "specials-round-trip.yaml", emitIntent(*result)));
      REQUIRE(again);
      CHECK(again->body == result->body);
    }

    SECTION("document size is bounded")
    {
      auto result = loadIntent(writeFile(temp, "large.yaml", std::string((2 * 1024 * 1024) + 1, 'x')));
      REQUIRE_FALSE(result);
      CHECK(result.error().message.find("2 MiB") != std::string::npos);
    }
  }

  TEST_CASE("Fleet intent - full override set round trips", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto intent = loadIntent(writeFile(temp, "base.yaml", intentYaml()));
    REQUIRE(intent);

    intent->overrides = IntentOverrides{.optAgent = "deepseek-pro",
                                        .optEngine = EngineKind::Synthesis,
                                        .optOracle = "test-core",
                                        .optRiskOracle = "test-delta",
                                        .optFanout = 2,
                                        .optTopK = 1,
                                        .optMaxRounds = 3,
                                        .optChurnLines = 100,
                                        .optDepth = CouncilDepth::Panel,
                                        .optQuorum = 2};

    auto again = loadIntent(writeFile(temp, "round.yaml", emitIntent(*intent)));
    REQUIRE(again);
    CHECK(again->overrides == intent->overrides);
  }

  TEST_CASE("Fleet YAML stream - control bytes in scalars round trip", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "stream.yaml";
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

  TEST_CASE("Fleet YAML stream - incomplete tail is recoverable", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "review-outcomes.yaml";
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
} // namespace ao::fleet::test
