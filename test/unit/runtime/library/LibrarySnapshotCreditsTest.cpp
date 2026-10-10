// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <optional>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("LibrarySnapshot - credit reads own canonical ordered entries and distinguish missing tracks",
            "[runtime][unit][library][readmodel]")
  {
    auto fixture = MusicLibraryFixture{};
    auto const expected = std::vector<library::Credit>{
      {.name = "Ada", .kind = library::CreditKind::Conductor, .role = "Guest"},
      {.name = "Ensemble", .kind = library::CreditKind::Ensemble},
      {.name = "Ada", .kind = library::CreditKind::Soloist, .role = "Piano"},
      {.name = "Ada", .role = "Piano"},
      {.name = "Bob"},
      {.name = "Ada", .role = "Piano"},
    };
    auto const interleaved = std::vector{expected[3], expected[2], expected[0], expected[4], expected[1], expected[5]};
    auto const trackId = fixture.addTrack(library::test::TrackSpec{.title = "Credits", .credits = interleaved});
    auto const emptyId = fixture.addTrack("No credits");
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto commands = LibraryCommandsFixture{fixture.library(), changes};
    auto optOwned = std::optional<std::vector<library::Credit>>{};
    auto optOwnedEmpty = std::optional<std::vector<library::Credit>>{};

    {
      auto const snapshot = commands.library().snapshot();
      optOwned = snapshot.trackCredits(trackId);
      optOwnedEmpty = snapshot.trackCredits(emptyId);
      REQUIRE(optOwnedEmpty);
      CHECK(optOwnedEmpty->empty());
      CHECK_FALSE(snapshot.trackCredits(TrackId{999999}));
    }

    REQUIRE(optOwned);
    CHECK(*optOwned == expected);
    REQUIRE(optOwnedEmpty);
    CHECK(optOwnedEmpty->empty());
  }

  TEST_CASE("LibrarySnapshot - credit reads remain bound to their committed revision",
            "[runtime][unit][library][readmodel]")
  {
    auto fixture = MusicLibraryFixture{};
    auto const original = std::vector<library::Credit>{
      {.name = "Conductor", .kind = library::CreditKind::Conductor}, {.name = "Ada", .role = "Piano"}};
    auto const replacement = std::vector<library::Credit>{
      {.name = "Conductor", .kind = library::CreditKind::Conductor}, {.name = "Bob", .role = "Violin"}};
    auto const trackId = fixture.addTrack(library::test::TrackSpec{.title = "Credits", .credits = original});
    auto changes = makeStateOnlyLibraryChanges(fixture.library());
    auto commands = LibraryCommandsFixture{fixture.library(), changes};
    auto const before = commands.library().snapshot();
    auto const revision = before.revision();

    REQUIRE(commands.updateMetadata(
      std::array{trackId},
      MetadataPatch{.optCredits = CreditReplacement{
                      .kinds = std::bitset<library::kCreditKindCount>{0b1000}, .entries = {replacement[1]}}}));

    auto const optRetained = before.trackCredits(trackId);
    REQUIRE(optRetained);
    CHECK(*optRetained == original);
    CHECK(before.revision() == revision);
    auto const after = commands.library().snapshot();
    CHECK(after.revision() == revision + 1);
    auto const optUpdated = after.trackCredits(trackId);
    REQUIRE(optUpdated);
    CHECK(*optUpdated == replacement);
  }
} // namespace ao::rt::test
