// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/uimodel/library/property/TrackAuthoringTestSupport.h"
#include <ao/Exception.h>
#include <ao/async/Executor.h>
#include <ao/async/Runtime.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    class SwallowingInlineExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return true; }

      void dispatch(std::move_only_function<void()> task) override
      {
        try
        {
          task();
        }
        catch (...)
        {
          // This executor intentionally models a callback boundary that consumes failures.
          return;
        }
      }

      void defer(std::move_only_function<void()> task) override { dispatch(std::move(task)); }
    };
  } // namespace

  TEST_CASE("TrackAuthoringSession - owns stable targets and becomes stale after another commit",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{2};
    auto const targetIds = std::array{fixture.trackIds()[1], fixture.trackIds()[0]};
    auto sessionResult = TrackAuthoringSession::begin(fixture.library(), targetIds);
    REQUIRE(sessionResult);
    auto sessionPtr = std::move(*sessionResult);

    CHECK(std::ranges::equal(sessionPtr->targetIds(), targetIds));
    CHECK(sessionPtr->state() == TrackAuthoringSessionState::Editing);

    auto states = std::vector<TrackAuthoringSessionState>{};
    auto subscription = sessionPtr->onStateChanged([&states](auto state) { states.push_back(state); });
    auto patch = rt::MetadataPatch{.optTitle = "Applied"};
    auto submitResult = sessionPtr->submitMetadata(patch);

    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::Applied);
    CHECK(sessionPtr->state() == TrackAuthoringSessionState::Applied);
    CHECK(fixture.title(targetIds[0]) == "Applied");
    CHECK(fixture.title(targetIds[1]) == "Applied");

    REQUIRE(fixture.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));
    CHECK(sessionPtr->state() == TrackAuthoringSessionState::Stale);
    CHECK(states == std::vector{TrackAuthoringSessionState::Submitting,
                                TrackAuthoringSessionState::Applied,
                                TrackAuthoringSessionState::Stale});

    patch.optTitle = "Must not apply";
    submitResult = sessionPtr->submitMetadata(patch);
    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::Stale);
    CHECK(fixture.title(targetIds[0]) == "Applied");
  }

  TEST_CASE("TrackAuthoringSession - semantic no-op keeps the binding usable", "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto sessionResult = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(sessionResult);
    auto sessionPtr = std::move(*sessionResult);

    auto submitResult = sessionPtr->submitMetadata(rt::MetadataPatch{.optTitle = "Old Title"});
    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::NoOp);
    CHECK(sessionPtr->state() == TrackAuthoringSessionState::Editing);

    submitResult = sessionPtr->submitMetadata(rt::MetadataPatch{.optTitle = "Now changed"});
    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::Applied);
    CHECK(fixture.title(fixture.trackIds().front()) == "Now changed");
  }

  TEST_CASE("TrackAuthoringSession - a tag commit stales other sessions bound to the old revision",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto firstResult = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    auto secondResult = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    auto firstPtr = std::move(*firstResult);
    auto secondPtr = std::move(*secondResult);

    auto submitResult = firstPtr->submitTags(std::array{std::string{"First"}}, {});

    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::Applied);
    CHECK(firstPtr->state() == TrackAuthoringSessionState::Applied);
    CHECK(secondPtr->state() == TrackAuthoringSessionState::Stale);
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"First"});

    submitResult = secondPtr->submitTags(std::array{std::string{"Second"}}, {});
    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::Stale);
    CHECK(fixture.tags(fixture.trackIds().front()) == std::vector<std::string>{"First"});
  }

  TEST_CASE("TrackAuthoringSession - post-commit publication failure makes the session stale",
            "[uimodel][unit][library-authoring]")
  {
    auto fixture = TrackAuthoringFixture{1};
    auto sessionResult = TrackAuthoringSession::begin(fixture.library(), fixture.trackIds());
    REQUIRE(sessionResult);
    auto sessionPtr = std::move(*sessionResult);
    auto throwingSubscription = fixture.changes().onChanged(
      [](rt::LibraryChangeSet const&) { throwException<Exception>("publication observer failed"); });

    CHECK_THROWS_AS(sessionPtr->submitMetadata(rt::MetadataPatch{.optTitle = "Committed"}), Exception);
    CHECK(sessionPtr->state() == TrackAuthoringSessionState::Stale);
    CHECK(fixture.title(fixture.trackIds().front()) == "Committed");
    CHECK(fixture.library().authoringAvailability().state == rt::LibraryAuthoringState::Faulted);
  }

  TEST_CASE("TrackAuthoringSession - swallowed publication failure retains applied outcome but stales the session",
            "[uimodel][unit][library-authoring]")
  {
    auto temp = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db");
    auto const trackId = library::test::addTrack(musicLibrary, library::test::TrackSpec{.title = "Before"});
    auto executor = SwallowingInlineExecutor{};
    auto asyncRuntime = async::Runtime{executor};
    auto initialRead = musicLibrary.readTransaction();
    auto changes = rt::LibraryChanges{executor, musicLibrary.libraryRevision(initialRead)};
    auto library = rt::Library{asyncRuntime, musicLibrary, changes};
    auto sessionResult = TrackAuthoringSession::begin(library, std::array{trackId});
    REQUIRE(sessionResult);
    auto sessionPtr = std::move(*sessionResult);
    auto throwingSubscription =
      changes.onChanged([](rt::LibraryChangeSet const&) { throwException<Exception>("publication observer failed"); });

    auto submitResult = sessionPtr->submitMetadata(rt::MetadataPatch{.optTitle = "Committed"});

    REQUIRE(submitResult);
    CHECK(submitResult->status == TrackAuthoringSubmitStatus::Applied);
    CHECK(sessionPtr->state() == TrackAuthoringSessionState::Stale);
    CHECK(library.authoringAvailability().state == rt::LibraryAuthoringState::Faulted);
  }
} // namespace ao::uimodel::test
