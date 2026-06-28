// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Model.h"
#include "fleet/Serialization.h"
#include "test/fleet/FleetTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace ao::fleet::test
{
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

  TEST_CASE("Fleet registry - rejects unknown agent references", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const position = source.find("agent: deepseek-pro");
    REQUIRE(position != std::string::npos);
    source.replace(position, std::string{"agent: deepseek-pro"}.size(), "agent: missing");

    auto result = loadRegistry(writeFile(temp, "missing-agent.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unknown agent") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects unknown harness references", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const position = source.find("harness: codex");
    REQUIRE(position != std::string::npos);
    source.replace(position, std::string{"harness: codex"}.size(), "harness: missing");

    auto result = loadRegistry(writeFile(temp, "missing-harness.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unknown harness") != std::string::npos);
  }

  TEST_CASE("Fleet registry - requires harness argv to include the model placeholder", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{", -m, \"{model}\"]"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "]");

    auto result = loadRegistry(writeFile(temp, "no-model.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("must use '{model}'") != std::string::npos);
  }

  TEST_CASE("Fleet registry - requires effort for effort-driven harnesses", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"\n    effort: xhigh"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "");

    auto result = loadRegistry(writeFile(temp, "missing-effort.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("must set effort") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects effort on harnesses that do not use it", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"vendor: google"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "vendor: google\n    effort: high");

    auto result = loadRegistry(writeFile(temp, "stray-effort.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("does not use '{effort}'") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects empty model identifiers", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"model: gpt-5.5"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "model: \"\"");

    auto result = loadRegistry(writeFile(temp, "empty-model.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("model must not be empty") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects placeholder braces inside model identifiers", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"model: gpt-5.5"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "model: \"gpt{prompt}\"");

    auto result = loadRegistry(writeFile(temp, "brace-model.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("placeholder braces") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects empty harness rate-limit keys", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"rate-limit-key: openai"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "rate-limit-key: \"\"");

    auto result = loadRegistry(writeFile(temp, "empty-rate-key.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("rate-limit-key must not be empty") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects unknown escalation routes", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"route: openai-gpt"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "route: missing");

    auto result = loadRegistry(writeFile(temp, "missing-route.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unknown agent") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects duplicate council roster members", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"roster: [anthropic-opus, openai-gpt, google-gemini-pro, deepseek-pro]"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(
      position, needle.size(), "roster: [anthropic-opus, anthropic-opus, google-gemini-pro, deepseek-pro]");

    auto result = loadRegistry(writeFile(temp, "duplicate-roster.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("twice") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects council rosters without cross-vendor coverage", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");

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

  TEST_CASE("Fleet registry - honors per-agent timeout overrides", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const needle = std::string{"vendor: openai"};
    auto const position = source.find(needle);
    REQUIRE(position != std::string::npos);
    source.replace(position, needle.size(), "vendor: openai\n    timeout-ms: 60000");

    auto result = loadRegistry(writeFile(temp, "timeout-override.yaml", source));

    REQUIRE(result);
    CHECK(result->agents.at("openai-gpt").timeout == std::chrono::minutes{1});
    CHECK(result->agents.at("deepseek-pro").timeout == std::chrono::minutes{20});
  }

  TEST_CASE("Fleet registry - rejects unknown argv placeholders", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const position = source.find("argv: [opencode");
    REQUIRE(position != std::string::npos);
    source.replace(position, std::string{"argv: [opencode"}.size(), "argv: [\"{bogus}\", opencode");

    auto result = loadRegistry(writeFile(temp, "placeholder.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unknown placeholder") != std::string::npos);
  }

  TEST_CASE("Fleet registry - rejects search bindings", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = readFile(std::filesystem::path{AOBUS_SOURCE_DIR} / "config/agent-fleet.yaml");
    auto const position = source.find("engine: gate");
    REQUIRE(position != std::string::npos);
    source.replace(position, std::string{"engine: gate"}.size(), "engine: search");

    auto result = loadRegistry(writeFile(temp, "search.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unsupported search engine") != std::string::npos);
  }
} // namespace ao::fleet::test
