// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace ao::uimodel::track::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("TrackPresentationViewModel - preset and layout management", "[uimodel][track][presentation]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playbackService = PlaybackService{executor, viewService, testLib.library()};
    auto workspace = rt::WorkspaceService{viewService, playbackService, changes, testLib.library()};

    auto store = TrackPresentationViewModel{workspace};

    SECTION("initial presets")
    {
      CHECK_FALSE(store.builtinPresets().empty());
    }

    SECTION("labels prefer builtin and custom names before falling back to the raw id")
    {
      store.addCustomPresentation(rt::CustomTrackPresentationPreset{
        .label = "Focused Metadata",
        .basePresetId = std::string{rt::kDefaultTrackPresentationId},
        .spec = rt::TrackPresentationSpec{.id = "focused-metadata"},
      });

      CHECK(store.labelForId(rt::kDefaultTrackPresentationId) == "Songs");
      CHECK(store.labelForId("focused-metadata") == "Focused Metadata");
      CHECK(store.labelForId("missing-preset") == "missing-preset");
    }

    SECTION("menu items list builtins, custom presets, separators, and create action")
    {
      store.addCustomPresentation(rt::CustomTrackPresentationPreset{
        .label = "Library QA",
        .basePresetId = std::string{rt::kDefaultTrackPresentationId},
        .spec = rt::TrackPresentationSpec{.id = "library-qa"},
      });

      auto const items = store.menuItems();
      auto const builtinCount = store.builtinPresets().size();

      REQUIRE(items.size() == builtinCount + 4);
      CHECK(items.front().type == TrackPresentationMenuItemType::Preset);
      CHECK(items.front().id == std::string{store.builtinPresets().front().spec.id});
      CHECK(items[builtinCount].type == TrackPresentationMenuItemType::Separator);
      CHECK(items[builtinCount + 1].type == TrackPresentationMenuItemType::Preset);
      CHECK(items[builtinCount + 1].id == "library-qa");
      CHECK(items[builtinCount + 1].label == "Library QA");
      CHECK(items[builtinCount + 2].type == TrackPresentationMenuItemType::Separator);
      CHECK(items.back().type == TrackPresentationMenuItemType::CreateCustomView);
      CHECK(items.back().label == "Create Custom View...");
    }

    SECTION("menu items omit the custom separator when there are no custom presets")
    {
      auto const items = store.menuItems();
      auto const builtinCount = store.builtinPresets().size();
      auto const separatorCount =
        std::ranges::count(items, TrackPresentationMenuItemType::Separator, &TrackPresentationMenuItem::type);

      REQUIRE(items.size() == builtinCount + 2);
      CHECK(separatorCount == 1);
      CHECK(items[builtinCount].type == TrackPresentationMenuItemType::Separator);
      CHECK(items.back().type == TrackPresentationMenuItemType::CreateCustomView);
    }

    SECTION("active presentation management")
    {
      store.setActivePresentationId("default");
      CHECK(store.activePresentationId() == "default");
    }

    SECTION("active presentation ignores unchanged ids")
    {
      std::int32_t changeCount = 0;
      auto sub = store.signalChanged().connect([&](auto, auto) { ++changeCount; });

      store.setActivePresentationId("albums");
      store.setActivePresentationId("albums");

      CHECK(store.activePresentationId() == "albums");
      CHECK(changeCount == 1);
    }

    SECTION("spec lookup supports custom presets and rejects unknown ids")
    {
      store.addCustomPresentation(rt::CustomTrackPresentationPreset{
        .label = "Movement Review",
        .basePresetId = std::string{rt::kDefaultTrackPresentationId},
        .spec =
          rt::TrackPresentationSpec{
            .id = "movement-review",
            .groupBy = rt::TrackGroupKey::Album,
            .visibleFields = {rt::TrackField::Title, rt::TrackField::MovementNumber},
          },
      });

      auto const optCustomSpec = store.specForId("movement-review");
      REQUIRE(optCustomSpec.has_value());
      CHECK(optCustomSpec->id == "movement-review");
      CHECK(optCustomSpec->groupBy == rt::TrackGroupKey::Album);

      CHECK_FALSE(store.specForId("not-a-presentation").has_value());
    }

    SECTION("list layout persistence")
    {
      auto const listId = rt::kAllTracksListId;
      auto const layout = std::vector{ColumnState{.field = rt::TrackField::Title, .width = 200}};

      store.updateLayout(listId, layout);

      auto const& retrieved = store.layoutForList(listId);
      REQUIRE(retrieved.size() == 1);
      CHECK(retrieved[0].field == rt::TrackField::Title);
      CHECK(retrieved[0].width == 200);
    }

    SECTION("layout updates ignore invalid and unchanged layouts")
    {
      auto const listId = rt::kAllTracksListId;
      auto const layout = std::vector{ColumnState{.field = rt::TrackField::Artist, .width = 180}};
      std::int32_t changeCount = 0;
      auto sub = store.signalChanged().connect([&](auto, auto) { ++changeCount; });

      store.updateLayout(kInvalidListId, layout);
      CHECK(store.layoutForList(kInvalidListId).empty());

      store.updateLayout(listId, layout);
      store.updateLayout(listId, layout);

      CHECK(changeCount == 1);
      CHECK(store.layoutForList(listId).front().field == rt::TrackField::Artist);
    }

    SECTION("active field order follows the active list layout")
    {
      auto const listId = rt::kAllTracksListId;
      store.updateLayout(listId,
                         {
                           ColumnState{.field = rt::TrackField::Album, .width = 230},
                           ColumnState{.field = rt::TrackField::Title, .width = 260},
                         });

      CHECK(store.activeFieldOrder().empty());

      store.setActiveListId(listId);
      auto const order = store.activeFieldOrder();

      REQUIRE(order.size() == 2);
      CHECK(order[0] == rt::TrackField::Album);
      CHECK(order[1] == rt::TrackField::Title);
    }

    SECTION("active list ignores unchanged ids")
    {
      std::int32_t changeCount = 0;
      auto sub = store.signalChanged().connect([&](auto, auto) { ++changeCount; });

      store.setActiveListId(rt::kAllTracksListId);
      store.setActiveListId(rt::kAllTracksListId);

      CHECK(changeCount == 1);
    }

    SECTION("bulk layout state emits only when changed")
    {
      auto const layouts = std::map<ListId, std::vector<ColumnState>>{
        {rt::kAllTracksListId, {ColumnState{.field = rt::TrackField::Duration, .width = 95}}},
      };
      std::int32_t changeCount = 0;
      auto sub = store.signalChanged().connect([&](auto, auto) { ++changeCount; });

      store.setListLayouts(layouts);
      store.setListLayouts(layouts);

      CHECK(changeCount == 1);
      CHECK(store.listLayouts().at(rt::kAllTracksListId).front().field == rt::TrackField::Duration);
    }

    SECTION("list presentation preferences")
    {
      auto const listId = rt::kAllTracksListId;

      CHECK_FALSE(store.presentationIdForList(listId));

      store.setPresentationIdForList(listId, "albums");

      auto const optId = store.presentationIdForList(listId);
      REQUIRE(optId);
      CHECK(*optId == "albums");
      CHECK(store.presentationForList(listId).id == "albums");

      store.clearPresentationForList(listId);
      CHECK_FALSE(store.presentationIdForList(listId));
    }

    SECTION("bulk presentation preferences emit only when changed")
    {
      auto const presentations = std::map<ListId, std::string>{{rt::kAllTracksListId, "albums"}};
      std::int32_t changeCount = 0;
      auto sub = store.signalChanged().connect([&](auto, auto) { ++changeCount; });

      store.setListPresentations(presentations);
      store.setListPresentations(presentations);

      CHECK(changeCount == 1);
      CHECK(store.listPresentations().at(rt::kAllTracksListId) == "albums");
    }

    SECTION("invalid list id presentation preference is ignored")
    {
      store.setPresentationIdForList(kInvalidListId, "albums");

      CHECK_FALSE(store.presentationIdForList(kInvalidListId));
    }

    SECTION("presentationForList uses custom preferences and falls back when preferences are unknown")
    {
      auto const listId = rt::kAllTracksListId;
      store.addCustomPresentation(rt::CustomTrackPresentationPreset{
        .label = "Tag Audit",
        .basePresetId = std::string{rt::kDefaultTrackPresentationId},
        .spec =
          rt::TrackPresentationSpec{.id = "tag-audit", .visibleFields = {rt::TrackField::Title, rt::TrackField::Tags}},
      });

      store.setPresentationIdForList(listId, "tag-audit");
      CHECK(store.presentationForList(listId).id == "tag-audit");

      store.setPresentationIdForList(listId, "missing-preset");
      CHECK(rt::builtinTrackPresentationPreset(store.presentationForList(listId).id) != nullptr);
    }

    SECTION("custom preset removal updates lookup results")
    {
      store.addCustomPresentation(rt::CustomTrackPresentationPreset{
        .label = "Temporary View",
        .basePresetId = std::string{rt::kDefaultTrackPresentationId},
        .spec = rt::TrackPresentationSpec{.id = "temporary-view"},
      });

      REQUIRE(store.specForId("temporary-view").has_value());

      store.removeCustomPresentation("temporary-view");

      CHECK_FALSE(store.specForId("temporary-view").has_value());
      CHECK(store.labelForId("temporary-view") == "temporary-view");
    }

    SECTION("signal propagation on change")
    {
      bool changed = false;
      auto sub = store.signalChanged().connect([&](auto, auto) { changed = true; });

      store.setActivePresentationId("modern");
      CHECK(changed == true);
    }
  }
} // namespace ao::uimodel::track::test
