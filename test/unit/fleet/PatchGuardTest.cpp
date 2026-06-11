// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Model.h"
#include "fleet/Substrate.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::fleet::test
{
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

    SECTION("declared paths operations and churn are enforced")
    {
      CHECK(PatchGuard::inspect(patch, scope, 10, {"tool/fleet"}).accepted);

      patch.touchedFiles = {TouchedFile{.path = "lib/audio/Other.cpp", .operation = ScopeOperation::Modify}};
      CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);

      patch.touchedFiles = {TouchedFile{.path = "lib/audio/Player.cpp", .operation = ScopeOperation::Modify}};
      patch.addedLines = 20;
      CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ChurnExceeded);

      patch.addedLines = 3;
      patch.touchedFiles = {TouchedFile{.path = "lib/audio/Player.cpp", .operation = ScopeOperation::Delete}};
      CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);
    }

    SECTION("ruler paths cannot be authorized by phase scope")
    {
      patch.touchedFiles = {TouchedFile{.path = "tool/fleet/Engine.cpp", .operation = ScopeOperation::Modify}};
      CHECK(
        PatchGuard::inspect(patch, {ScopeRule{"tool/fleet/Engine.cpp", {ScopeOperation::Modify}}}, 10, {"tool/fleet"})
          .failure == FailureReason::ScopeViolation);

      patch.touchedFiles = {TouchedFile{.path = "lib/audio/CMakeLists.txt", .operation = ScopeOperation::Modify}};
      CHECK(
        PatchGuard::inspect(patch, {ScopeRule{"lib/audio/CMakeLists.txt", {ScopeOperation::Modify}}}, 10, {}).failure ==
        FailureReason::ScopeViolation);
    }

    SECTION("rename and mode metadata are rejected only as diff directives")
    {
      patch.patch = "diff --git a/lib/audio/Player.cpp b/lib/audio/Player.cpp\n"
                    "+  // documentation: rename from X, copy from Y, old mode bits\n";
      CHECK(PatchGuard::inspect(patch, scope, 10, {}).accepted);

      patch.patch += "old mode 100644\nnew mode 100755\n";
      CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);

      patch.patch = "diff --git a/x b/x\nrename from lib/audio/Player.cpp\n";
      CHECK(PatchGuard::inspect(patch, scope, 10, {}).failure == FailureReason::ScopeViolation);
    }
  }
} // namespace ao::fleet::test
