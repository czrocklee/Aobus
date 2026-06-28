// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Model.h"
#include "fleet/Serialization.h"
#include "test/fleet/FleetTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::fleet::test
{
  TEST_CASE("Fleet intent - round trips through strict schema", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};

    auto first = loadIntent(writeFile(temp, "first.yaml", intentYaml()));
    REQUIRE(first);
    auto second = loadIntent(writeFile(temp, "second.yaml", emitIntent(*first)));
    REQUIRE(second);
    CHECK(second->id == first->id);
    CHECK(second->scope == first->scope);
    CHECK(second->body == first->body);
  }

  TEST_CASE("Fleet intent - rejects unknown keys", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "surprise: true\n";

    auto result = loadIntent(writeFile(temp, "unknown.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unknown field") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects duplicate keys", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "body: duplicate\n";

    auto result = loadIntent(writeFile(temp, "duplicate.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("duplicate key") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects scope path traversal", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};

    auto result = loadIntent(writeFile(temp, "traversal.yaml", intentYaml("phase-a", "", "../outside.cpp")));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("traversal") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects artifact id traversal", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};

    auto result = loadIntent(writeFile(temp, "id-traversal.yaml", intentYaml("../escape")));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("unsafe") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects anchors", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "extra: &anchor value\n";

    auto result = loadIntent(writeFile(temp, "anchor.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("anchors") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects aliases", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "extra: *anchor\n";

    auto result = loadIntent(writeFile(temp, "alias.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("aliases") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects tags", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "extra: !!str value\n";

    auto result = loadIntent(writeFile(temp, "tag.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("tags") != std::string::npos);
  }

  TEST_CASE("Fleet intent - rejects merge keys", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "<<: {extra: value}\n";

    auto result = loadIntent(writeFile(temp, "merge.yaml", source));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("merge keys") != std::string::npos);
  }

  TEST_CASE("Fleet intent - accepts YAML-like characters inside scalar content", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto source = intentYaml();
    source += "  Run *Test.cpp suites, keep !queue.empty() checks, pass &config by reference.\n";

    auto result = loadIntent(writeFile(temp, "specials.yaml", source));

    REQUIRE(result);
    CHECK(result->body.find("*Test.cpp") != std::string::npos);

    auto again = loadIntent(writeFile(temp, "specials-round-trip.yaml", emitIntent(*result)));
    REQUIRE(again);
    CHECK(again->body == result->body);
  }

  TEST_CASE("Fleet intent - rejects oversized documents", "[fleet][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};

    auto result = loadIntent(writeFile(temp, "large.yaml", std::string((2 * 1024 * 1024) + 1, 'x')));

    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("2 MiB") != std::string::npos);
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
} // namespace ao::fleet::test
