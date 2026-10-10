// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/LoopExecutor.h>
#include <ao/library/Credits.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/rt/source/TrackSourceCache.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kPerformer = static_cast<std::size_t>(library::CreditKind::Performer);
    constexpr auto kConductor = static_cast<std::size_t>(library::CreditKind::Conductor);

    std::vector<library::Credit> gouldPianoSmith()
    {
      return {{.name = "Glenn Gould", .role = "Piano"}, {.name = "Jane Smith"}};
    }

    struct TrackDetailPerformanceFixture final
    {
      MusicLibraryFixture libraryFixture;
      async::LoopExecutor executor;
      LibraryChanges changes;
      LibraryCommandsFixture commandsFixture;
      TrackSourceCache sources;
      ViewService views;
      WorkspaceService workspace;

      TrackDetailPerformanceFixture()
        : libraryFixture{}
        , changes{executor, 0, "test-library"}
        , commandsFixture{libraryFixture.library(), changes, executor}
        , sources{libraryFixture.library(), changes}
        , views{executor, libraryFixture.library(), sources, changes}
        , workspace{executor, views, changes}
      {
      }

      TrackId addTrack(library::test::TrackSpec const& spec) { return commandsFixture.addTrack(spec); }

      ViewId allTracksView()
      {
        auto const reply = ao::test::requireValue(workspace.navigate({.target = kAllTracksListId}));
        return reply;
      }
    };
  } // namespace

  TEST_CASE("TrackDetailPerformance - single selection resolves the owning ordered section",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto const id = env.addTrack(library::test::TrackSpec{.title = "Goldberg", .credits = gouldPianoSmith()});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto const snap = projPtr->snapshot();

    CHECK(snap.selectionKind == SelectionKind::Single);
    REQUIRE(snap.credits[kPerformer].optValue);
    CHECK(*snap.credits[kPerformer].optValue == gouldPianoSmith());
    CHECK_FALSE(snap.credits[kPerformer].mixed);
  }

  TEST_CASE("TrackDetailPerformance - an empty single list has present common sections",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto const id = env.addTrack(library::test::TrackSpec{.title = "No credits"});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto const snap = projPtr->snapshot();

    for (auto const& section : snap.credits)
    {
      REQUIRE(section.optValue);
      CHECK(section.optValue->empty());
      CHECK_FALSE(section.mixed);
    }
  }

  TEST_CASE("TrackDetailPerformance - identical lists aggregate to one common section",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto const id1 = env.addTrack(library::test::TrackSpec{.title = "First", .credits = gouldPianoSmith()});
    auto const id2 = env.addTrack(library::test::TrackSpec{.title = "Second", .credits = gouldPianoSmith()});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id1, id2}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto const snap = projPtr->snapshot();

    CHECK(snap.selectionKind == SelectionKind::Multiple);
    REQUIRE(snap.credits[kPerformer].optValue);
    CHECK(*snap.credits[kPerformer].optValue == gouldPianoSmith());
    CHECK_FALSE(snap.credits[kPerformer].mixed);
  }

  TEST_CASE("TrackDetailPerformance - identical empty lists aggregate to common empty sections",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto const id1 = env.addTrack(library::test::TrackSpec{.title = "First"});
    auto const id2 = env.addTrack(library::test::TrackSpec{.title = "Second"});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id1, id2}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto const snap = projPtr->snapshot();

    for (auto const& section : snap.credits)
    {
      REQUIRE(section.optValue);
      CHECK(section.optValue->empty());
      CHECK_FALSE(section.mixed);
    }
  }

  TEST_CASE("TrackDetailPerformance - order role multiplicity and absence differences report mixed sections",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto differing = std::vector<library::Credit>{};

    SECTION("Within-kind order")
    {
      differing = {{.name = "Jane Smith"}, {.name = "Glenn Gould", .role = "Piano"}};
    }

    SECTION("Role only")
    {
      differing = {{.name = "Glenn Gould", .role = "Harpsichord"}, {.name = "Jane Smith"}};
    }

    SECTION("Multiplicity")
    {
      differing = {
        {.name = "Glenn Gould", .role = "Piano"}, {.name = "Jane Smith"}, {.name = "Glenn Gould", .role = "Piano"}};
    }

    SECTION("Empty versus nonempty")
    {
      differing.clear();
    }

    auto const id1 = env.addTrack(library::test::TrackSpec{.title = "First", .credits = gouldPianoSmith()});
    auto const id2 = env.addTrack(library::test::TrackSpec{.title = "Second", .credits = differing});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id1, id2}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto const snap = projPtr->snapshot();

    CHECK(snap.credits[kPerformer].mixed);
    CHECK_FALSE(snap.credits[kPerformer].optValue);
  }

  TEST_CASE("TrackDetailPerformance - common complete conductors remain available with mixed performers",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto const conductors =
      std::vector<library::Credit>{{.name = "First", .kind = library::CreditKind::Conductor, .role = "Guest"},
                                   {.name = "Second", .kind = library::CreditKind::Conductor},
                                   {.name = "First", .kind = library::CreditKind::Conductor, .role = "Guest"}};
    auto first = conductors;
    first.push_back({.name = "Ada"});
    auto second = conductors;
    second.push_back({.name = "Bob"});
    auto const id1 = env.addTrack(library::test::TrackSpec{.title = "First", .credits = first});
    auto const id2 = env.addTrack(library::test::TrackSpec{.title = "Second", .credits = second});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id1, id2}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto const snap = projPtr->snapshot();

    CHECK(snap.credits[kPerformer].mixed);
    CHECK_FALSE(snap.credits[kPerformer].optValue);
    REQUIRE(snap.credits[kConductor].optValue);
    CHECK(*snap.credits[kConductor].optValue == conductors);
    CHECK_FALSE(snap.credits[kConductor].mixed);
  }

  TEST_CASE("TrackDetailPerformance - no selection keeps every section absent",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    env.addTrack(library::test::TrackSpec{.title = "Ignored"});
    auto projPtr = env.workspace.detailProjection(ExplicitSelectionTarget{.trackIds = {}});
    auto const snap = projPtr->snapshot();

    CHECK(snap.selectionKind == SelectionKind::None);

    for (auto const& section : snap.credits)
    {
      CHECK_FALSE(section.optValue);
      CHECK_FALSE(section.mixed);
    }
  }

  TEST_CASE("TrackDetailPerformance - refreshes the credit section after an intersecting mutation",
            "[runtime][unit][projection][track-detail]")
  {
    auto env = TrackDetailPerformanceFixture{};
    auto const id = env.addTrack(library::test::TrackSpec{.title = "Goldberg", .credits = gouldPianoSmith()});
    auto const viewId = env.allTracksView();
    REQUIRE(env.views.setSelection(viewId, {id}));
    auto projPtr = env.workspace.detailProjection(ExplicitViewTarget{viewId});
    auto snap = projPtr->snapshot();
    REQUIRE(snap.credits[kPerformer].optValue);
    CHECK(*snap.credits[kPerformer].optValue == gouldPianoSmith());

    auto const replacement = std::vector<library::Credit>{{.name = "Ada", .role = "Violin"}, {.name = "Ada"}};
    auto const targetIds = std::array{id};
    REQUIRE(env.commandsFixture.updateMetadata(
      targetIds, MetadataPatch{.optCredits = CreditReplacement{.kinds = 8, .entries = replacement}}));
    snap = projPtr->snapshot();
    REQUIRE(snap.credits[kPerformer].optValue);
    CHECK(*snap.credits[kPerformer].optValue == replacement);
    CHECK_FALSE(snap.credits[kPerformer].mixed);

    REQUIRE(env.commandsFixture.updateMetadata(targetIds, MetadataPatch{.optCredits = CreditReplacement{.kinds = 8}}));
    snap = projPtr->snapshot();
    REQUIRE(snap.credits[kPerformer].optValue);
    CHECK(snap.credits[kPerformer].optValue->empty());
  }
} // namespace ao::rt::test
