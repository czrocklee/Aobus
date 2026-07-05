// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/runtime/ViewServiceTestSupport.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  TEST_CASE("ViewService - createView with groupBy applies effective sort", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({.groupBy = TrackGroupKey::Artist}, true);
    auto const snap = service.trackListState(result.viewId);

    CHECK(snap.groupBy == TrackGroupKey::Artist);

    auto const expected = std::vector{TrackSortField::Artist,
                                      TrackSortField::Year,
                                      TrackSortField::Album,
                                      TrackSortField::DiscNumber,
                                      TrackSortField::TrackNumber,
                                      TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
      CHECK(snap.sortBy[i].ascending == true);
    }
  }

  TEST_CASE("ViewService - createView with Album groupBy applies album sort", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({.groupBy = TrackGroupKey::Album}, true);
    auto const snap = service.trackListState(result.viewId);

    CHECK(snap.groupBy == TrackGroupKey::Album);

    auto const expected = std::vector{TrackSortField::AlbumArtist,
                                      TrackSortField::Album,
                                      TrackSortField::DiscNumber,
                                      TrackSortField::TrackNumber,
                                      TrackSortField::Title};
    REQUIRE(snap.sortBy.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
      CHECK(snap.sortBy[i].field == expected[i]);
    }
  }

  TEST_CASE("ViewService - setPresentation updates state and projection", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);
    auto const viewId = ViewId{result.viewId};

    auto const* preset = builtinTrackPresentationPreset("genres");
    REQUIRE(preset != nullptr);
    service.setPresentation(viewId, preset->spec);
    auto const snap = service.trackListState(viewId);

    CHECK(snap.groupBy == TrackGroupKey::Genre);
    CHECK(snap.presentation.id == "genres");
  }

  TEST_CASE("ViewService - setPresentation no-ops on same value", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();

    auto const* preset = builtinTrackPresentationPreset("years");
    REQUIRE(preset != nullptr);
    auto const result = service.createView({}, true);
    service.setPresentation(result.viewId, preset->spec);
    auto const snap = service.trackListState(result.viewId);
    auto const revBefore = snap.revision;

    service.setPresentation(result.viewId, preset->spec);
    auto const snapAfter = service.trackListState(result.viewId);

    CHECK(snapAfter.revision == revBefore);
    CHECK(snapAfter.groupBy == TrackGroupKey::Year);
  }

  TEST_CASE("ViewService - setPresentation publishes PresentationChanged", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);

    auto received = TrackPresentationSpec{};
    auto const sub = service.onPresentationChanged([&](auto const& ev) { received = ev.presentation; });

    auto const* preset = builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    service.setPresentation(result.viewId, preset->spec);

    CHECK(received.id == "albums");
    CHECK(received.groupBy == TrackGroupKey::Album);
  }

  TEST_CASE("ViewService - setPresentation no-op does not publish event", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();

    auto const result = service.createView({}, true);

    std::int32_t callCount = 0;
    auto const sub = service.onPresentationChanged([&](auto const&) { ++callCount; });

    auto const* artistPreset = builtinTrackPresentationPreset("artists");
    REQUIRE(artistPreset != nullptr);
    service.setPresentation(result.viewId, artistPreset->spec);
    CHECK(callCount == 1);

    service.setPresentation(result.viewId, artistPreset->spec);
    CHECK(callCount == 1);

    auto const* albumPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumPreset != nullptr);
    service.setPresentation(result.viewId, albumPreset->spec);
    CHECK(callCount == 2);
  }

  TEST_CASE("ViewService - setPresentation with preset string", "[runtime][unit][view][presentation]")
  {
    auto env = ViewServiceTestEnv{};
    auto service = env.makeService();
    auto const result = service.createView({}, true);

    auto const spec = service.setPresentation(result.viewId, "artists");
    auto const snap = service.trackListState(result.viewId);

    CHECK(spec.id == "artists");
    CHECK(snap.groupBy == TrackGroupKey::AlbumArtist);

    auto const specInv = service.setPresentation(ViewId{999}, "artists");
    CHECK(specInv.id.empty());
  }
} // namespace ao::rt::test
