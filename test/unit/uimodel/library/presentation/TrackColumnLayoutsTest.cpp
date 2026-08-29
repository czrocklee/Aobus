// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackColumnLayouts - stores layouts and emits only for changes", "[uimodel][unit][library][presentation]")
  {
    auto store = TrackColumnLayouts{};
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const layout = std::vector{TrackColumnState{.field = rt::TrackField::Album, .width = 230, .weight = -1.0},
                                    TrackColumnState{.field = rt::TrackField::Title, .width = -1, .weight = 1.25}};

    store.updateLayout(kInvalidListId, layout);
    CHECK(store.layoutForList(kInvalidListId).empty());
    CHECK(events.empty());

    store.updateLayout(rt::kAllTracksListId, layout);
    store.updateLayout(rt::kAllTracksListId, layout);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == rt::kAllTracksListId);
    REQUIRE(store.layoutForList(rt::kAllTracksListId).size() == 2);
    CHECK(store.layoutForList(rt::kAllTracksListId)[0].field == rt::TrackField::Album);
    CHECK(store.layoutForList(rt::kAllTracksListId)[0].width == 230);
    CHECK(store.layoutForList(rt::kAllTracksListId)[1].field == rt::TrackField::Title);
    CHECK(store.layoutForList(rt::kAllTracksListId)[1].weight == 1.25);
  }

  TEST_CASE("TrackColumnLayouts - bulk state emits only when changed", "[uimodel][unit][library][presentation]")
  {
    auto store = TrackColumnLayouts{};
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) noexcept { events.push_back(listId); });
    auto const layouts = std::map<ListId, std::vector<TrackColumnState>>{
      {rt::kAllTracksListId, {TrackColumnState{.field = rt::TrackField::Duration, .width = 95, .weight = -1.0}}},
    };

    store.restore(layouts);
    store.restore(layouts);

    REQUIRE(events.size() == 1);
    CHECK(events[0] == kInvalidListId);
    REQUIRE(store.snapshot().at(rt::kAllTracksListId).size() == 1);
    CHECK(store.snapshot().at(rt::kAllTracksListId).front().field == rt::TrackField::Duration);
    CHECK(store.snapshot().at(rt::kAllTracksListId).front().width == 95);
  }
} // namespace ao::uimodel::test
