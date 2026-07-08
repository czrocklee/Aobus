// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/CouncilSchema.h"
#include "council/Serialization.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace ao::council::test
{
  TEST_CASE("Intent - focus hints parse and emit", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "intent.yaml",
                                R"(schema: aobus-council-intent/v1
id: phase-a
task-kind: council-review
invariant: Preserve playback behavior.
focus:
  - path: app/audio/
depends-on: []
overrides:
  roster: [agent-a, agent-b]
  depth: panel
  quorum: 2
body: |
  Review the change.
)");

    auto intent = loadIntent(path);
    REQUIRE(intent);
    CHECK(intent->id == "phase-a");
    CHECK(intent->taskKind == "council-review");
    REQUIRE(intent->focus.size() == 1);
    CHECK(intent->focus.front().path == std::filesystem::path{"app/audio"});
    CHECK(intent->focus.front().match == FocusMatch::Prefix);
    REQUIRE(intent->overrides.optRoster);
    CHECK(intent->overrides.optRoster->size() == 2);
    CHECK(intent->overrides.optDepth == Depth::Panel);
    CHECK(intent->overrides.optQuorum == 2);

    auto const emittedPath = writeFile(temp, "roundtrip.yaml", emitIntent(*intent));
    auto roundTrip = loadIntent(emittedPath);
    REQUIRE(roundTrip);
    CHECK(roundTrip->focus == intent->focus);
    CHECK(roundTrip->overrides == intent->overrides);
  }

  TEST_CASE("Intent - omitted focus is repository-wide advisory context", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "intent.yaml",
                                R"(schema: aobus-council-intent/v1
id: phase-a
task-kind: council-review
invariant: Preserve behavior.
depends-on: []
overrides: {}
body: |
  Review the whole change.
)");

    auto intent = loadIntent(path);
    REQUIRE(intent);
    CHECK(intent->focus.empty());
  }

  TEST_CASE("Intent - rejects removed scope field", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "intent.yaml",
                                R"(schema: aobus-council-intent/v1
id: phase-a
task-kind: council-review
invariant: Preserve behavior.
scope:
  - path: src
depends-on: []
overrides: {}
body: |
  Review the change.
)");

    auto intent = loadIntent(path);
    REQUIRE_FALSE(intent);
    CHECK(intent.error().message.find("unknown field 'scope'") != std::string::npos);
  }

  TEST_CASE("Intent - rejects path-reserved phase ids", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "intent.yaml",
                                R"(schema: aobus-council-intent/v1
id: ..
task-kind: council-review
invariant: Preserve behavior.
depends-on: []
overrides: {}
body: |
  Review the change.
)");

    auto intent = loadIntent(path);
    REQUIRE_FALSE(intent);
    CHECK(intent.error().message.find("reserved") != std::string::npos);
  }

  TEST_CASE("Intent - rejects path traversal in focus hints", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = writeFile(temp,
                                "intent.yaml",
                                R"(schema: aobus-council-intent/v1
id: phase-a
task-kind: council-review
invariant: Preserve behavior.
focus:
  - path: ../outside
depends-on: []
overrides: {}
body: |
  Review the change.
)");

    auto intent = loadIntent(path);
    REQUIRE_FALSE(intent);
    CHECK(intent.error().message.find("traverse outside") != std::string::npos);
  }
} // namespace ao::council::test
