// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestUtils.h"
#include <ao/query/Field.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - preserves field enum values", "[query][unit][execution_plan][contract]")
  {
    // String fields
    CHECK(static_cast<std::uint8_t>(Field::Title) == 0);
    CHECK(static_cast<std::uint8_t>(Field::Uri) == 1);

    // Property fields
    CHECK(static_cast<std::uint8_t>(Field::Duration) == 2);
    CHECK(static_cast<std::uint8_t>(Field::Bitrate) == 3);
    CHECK(static_cast<std::uint8_t>(Field::SampleRate) == 4);
    CHECK(static_cast<std::uint8_t>(Field::Channels) == 5);
    CHECK(static_cast<std::uint8_t>(Field::BitDepth) == 6);
    CHECK(static_cast<std::uint8_t>(Field::Codec) == 7);

    // Metadata ID fields
    CHECK(static_cast<std::uint8_t>(Field::ArtistId) == 9);
    CHECK(static_cast<std::uint8_t>(Field::AlbumId) == 10);
    CHECK(static_cast<std::uint8_t>(Field::ComposerId) == 13);
    CHECK(static_cast<std::uint8_t>(Field::WorkId) == 15);

    // Metadata numeric fields
    CHECK(static_cast<std::uint8_t>(Field::Year) == 16);
    CHECK(static_cast<std::uint8_t>(Field::TrackNumber) == 17);

    // Tag fields
    CHECK(static_cast<std::uint8_t>(Field::TagBloom) == 21);
    CHECK(static_cast<std::uint8_t>(Field::TagCount) == 22);

    // Cold classical metadata fields
    CHECK(static_cast<std::uint8_t>(Field::MovementId) == 25);
    CHECK(static_cast<std::uint8_t>(Field::MovementNumber) == 26);
    CHECK(static_cast<std::uint8_t>(Field::MovementTotal) == 27);
    CHECK(static_cast<std::uint8_t>(Field::ConductorId) == 28);
    CHECK(static_cast<std::uint8_t>(Field::EnsembleId) == 29);
    CHECK(static_cast<std::uint8_t>(Field::SoloistId) == 30);
  }

  TEST_CASE("ExecutionPlan - preserves opcode enum values", "[query][unit][execution_plan][contract]")
  {
    CHECK(static_cast<std::uint8_t>(OpCode::Nop) == 0);
    CHECK(static_cast<std::uint8_t>(OpCode::LoadField) == 1);
    CHECK(static_cast<std::uint8_t>(OpCode::LoadConstant) == 2);
    CHECK(static_cast<std::uint8_t>(OpCode::Eq) == 3);
    CHECK(static_cast<std::uint8_t>(OpCode::Ne) == 4);
  }

  TEST_CASE("compileQuery returns Result without throwing", "[query][unit][execution_plan][contract]")
  {
    SECTION("Valid predicate yields a plan")
    {
      auto const plan = compileQuery(parseOk("$year = 1990"));
      REQUIRE(plan.has_value());
      CHECK(plan->accessProfile != AccessProfile::NoTrackData);
    }

    SECTION("Non-predicate expression yields an Error")
    {
      auto const plan = compileQuery(parseOk("$year"));
      REQUIRE_FALSE(plan.has_value());
      CHECK(plan.error().code == Error::Code::FormatRejected);
      CHECK_FALSE(plan.error().message.empty());
    }
  }
} // namespace ao::query::test
