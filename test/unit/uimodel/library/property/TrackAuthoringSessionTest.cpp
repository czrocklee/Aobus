// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include "test/unit/uimodel/library/property/TrackAuthoringTestSupport.h"
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackAuthoringSession - owns stable targets and becomes stale after another commit",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const targetIds = std::array{fixture.trackIds()[1], fixture.trackIds()[0]};
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), targetIds);
    REQUIRE(sessionRes);
    auto sessionPtr = std::move(*sessionRes);

    CHECK(std::ranges::equal(sessionPtr->targetIds(), targetIds));
    CHECK(sessionPtr->isCurrent());

    std::size_t invalidatedCount = 0;
    auto subscription = sessionPtr->onInvalidated([&invalidatedCount] noexcept { ++invalidatedCount; });
    auto patch = rt::MetadataPatch{.optTitle = "Applied"};
    auto submitRes = sessionPtr->submitMetadata(patch);

    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::TrackAuthoringStatus::Applied);
    CHECK(sessionPtr->isCurrent());
    CHECK(invalidatedCount == 0);
    CHECK(fixture.title(targetIds[0]) == "Applied");
    CHECK(fixture.title(targetIds[1]) == "Applied");

    REQUIRE(fixture.library().writer().createList(rt::LibraryWriter::ListDraft{.name = "Unrelated"}));
    CHECK_FALSE(sessionPtr->isCurrent());
    CHECK(invalidatedCount == 1);

    patch.optTitle = "Must not apply";
    submitRes = sessionPtr->submitMetadata(patch);
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::TrackAuthoringStatus::Stale);
    CHECK(fixture.title(targetIds[0]) == "Applied");
  }

  TEST_CASE("TrackAuthoringSession - semantic no-op keeps the binding usable", "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto sessionRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(sessionRes);
    auto sessionPtr = std::move(*sessionRes);

    auto submitRes = sessionPtr->submitMetadata(rt::MetadataPatch{.optTitle = "Old Title"});
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::TrackAuthoringStatus::NoOp);
    CHECK(sessionPtr->isCurrent());

    submitRes = sessionPtr->submitMetadata(rt::MetadataPatch{.optTitle = "Now changed"});
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::TrackAuthoringStatus::Applied);
    CHECK(fixture.title(fixture.trackIds().front()) == "Now changed");
  }

  TEST_CASE("TrackAuthoringSession - a tag commit stales other sessions bound to the old revision",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto firstRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    auto secondRes = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(firstRes);
    REQUIRE(secondRes);
    auto firstPtr = std::move(*firstRes);
    auto secondPtr = std::move(*secondRes);

    auto submitRes = firstPtr->submitTags(std::array{std::string{"First"}}, {});

    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::TrackAuthoringStatus::Applied);
    CHECK(firstPtr->isCurrent());
    CHECK_FALSE(secondPtr->isCurrent());
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"First"});

    submitRes = secondPtr->submitTags(std::array{std::string{"Second"}}, {});
    REQUIRE(submitRes);
    CHECK(submitRes->status == rt::TrackAuthoringStatus::Stale);
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"First"});
  }
} // namespace ao::uimodel::test
