// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Model.h"
#include "fleet/Serialization.h"
#include "test/fleet/FleetTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <ios>
#include <string>

namespace ao::fleet::test
{
  TEST_CASE("Fleet review verdict - parser accepts only protocol values", "[fleet][unit][yaml]")
  {
    CHECK(parseReviewVerdict("accept") == ReviewVerdict::Accept);
    CHECK(parseReviewVerdict("modify") == ReviewVerdict::Modify);
    CHECK(parseReviewVerdict("reject") == ReviewVerdict::Reject);
    CHECK_FALSE(parseReviewVerdict("approve").has_value());
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
