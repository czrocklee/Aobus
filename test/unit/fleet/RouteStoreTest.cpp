// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/RouteStore.h"

#include "fleet/Model.h"
#include "fleet/Serialization.h"
#include "test/fleet/FleetTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <optional>

namespace ao::fleet::test
{
  TEST_CASE("Fleet route store - breaker uses five-result window and reset", "[fleet][unit][route]")
  {
    auto temp = ao::test::TempDir{};
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
    auto store = RouteStore{tempPath(temp)};

    for (std::int32_t index = 0; index < 5; ++index)
    {
      manifest.phaseId = std::format("phase-{}", index);
      writeFile(temp, manifest.phaseId + "/manifest.yaml", emitManifest(manifest));
      auto const verdict = index < 3 ? ReviewVerdict::Reject : ReviewVerdict::Accept;
      REQUIRE(store.record(manifest.phaseId, verdict, "test outcome"));
    }

    auto pausedResult = store.paused(manifest.route.canonical());
    REQUIRE(pausedResult);
    CHECK(*pausedResult);

    SECTION("reset resumes a paused route")
    {
      REQUIRE(store.reset(manifest.route.canonical()));
      pausedResult = store.paused(manifest.route.canonical());
      REQUIRE(pausedResult);
      CHECK_FALSE(*pausedResult);
    }

    SECTION("terminal reviews cannot be overwritten")
    {
      auto duplicate = store.record("phase-0", ReviewVerdict::Modify, "conflicting outcome");
      REQUIRE_FALSE(duplicate);
      CHECK(duplicate.error().message.find("terminal") != std::string::npos);
    }
  }
} // namespace ao::fleet::test
