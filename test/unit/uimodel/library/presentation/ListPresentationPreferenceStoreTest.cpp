// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/library/presentation/TrackPresentationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("ListPresentationPreferenceStore - stores list presentation ids and emits changed lists",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) { events.push_back(listId); });

    store.setPresentationIdForList(kInvalidListId, "albums");
    CHECK_FALSE(store.presentationIdForList(kInvalidListId));
    CHECK(events.empty());

    store.setPresentationIdForList(rt::kAllTracksListId, "albums");
    store.setPresentationIdForList(rt::kAllTracksListId, "albums");

    auto const optId = store.presentationIdForList(rt::kAllTracksListId);
    REQUIRE(optId);
    CHECK(*optId == "albums");
    REQUIRE(events.size() == 1);
    CHECK(events[0] == rt::kAllTracksListId);

    store.clearPresentationForList(rt::kAllTracksListId);

    CHECK_FALSE(store.presentationIdForList(rt::kAllTracksListId));
    REQUIRE(events.size() == 2);
    CHECK(events[1] == rt::kAllTracksListId);
  }

  TEST_CASE("ListPresentationPreferenceStore - empty presentation id clears without inserting empty state",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) { events.push_back(listId); });

    store.setPresentationIdForList(rt::kAllTracksListId, "");
    CHECK(store.listPresentations().empty());
    CHECK(events.empty());

    store.setPresentationIdForList(rt::kAllTracksListId, "albums");
    store.setPresentationIdForList(rt::kAllTracksListId, "");

    CHECK(store.listPresentations().empty());
    REQUIRE(events.size() == 2);
    CHECK(events[0] == rt::kAllTracksListId);
    CHECK(events[1] == rt::kAllTracksListId);
  }

  TEST_CASE("ListPresentationPreferenceStore - resolves custom preferences and falls back for unknown ids",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    fixture.catalog.addCustomPresentation(rt::CustomTrackPresentationPreset{
      .label = "Tag Audit",
      .basePresetId = std::string{rt::kDefaultTrackPresentationId},
      .spec =
        rt::TrackPresentationSpec{.id = "tag-audit", .visibleFields = {rt::TrackField::Title, rt::TrackField::Tags}},
    });

    store.setPresentationIdForList(rt::kAllTracksListId, "tag-audit");
    CHECK(store.presentationForList(rt::kAllTracksListId).id == "tag-audit");

    store.setPresentationIdForList(rt::kAllTracksListId, "missing-preset");
    CHECK(rt::builtinTrackPresentationPreset(store.presentationForList(rt::kAllTracksListId).id) != nullptr);
  }

  TEST_CASE("ListPresentationPreferenceStore - bulk state emits only when changed",
            "[uimodel][unit][library][presentation]")
  {
    auto fixture = TrackPresentationFixture{};
    auto& store = fixture.preferences;
    auto events = std::vector<ListId>{};
    auto sub = store.signalChanged().connect([&events](ListId listId) { events.push_back(listId); });
    auto const presentations = std::map<ListId, std::string>{{rt::kAllTracksListId, "albums"}};

    store.setListPresentations(presentations);
    store.setListPresentations(presentations);

    REQUIRE(events.size() == 1);
    CHECK(events[0] == kInvalidListId);
    CHECK(store.listPresentations().at(rt::kAllTracksListId) == "albums");
  }
} // namespace ao::uimodel::test
