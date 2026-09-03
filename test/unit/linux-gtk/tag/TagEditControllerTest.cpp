// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "app/ThemeCoordinator.h"
#include "image/ImageCache.h"
#include "image/ResourceImageLoader.h"
#include "tag/TagEditor.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <ao/CoreIds.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>
#include <gio/gio.h>
#include <gtk/gtkpopovermenu.h>
#include <gtk/gtkwidget.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    std::optional<std::string> menuLabelForAction(GMenuModel* const model, std::string_view const action)
    {
      if (model == nullptr)
      {
        return std::nullopt;
      }

      auto const itemCount = ::g_menu_model_get_n_items(model);

      for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex)
      {
        auto* const actionValue =
          ::g_menu_model_get_item_attribute_value(model, itemIndex, G_MENU_ATTRIBUTE_ACTION, G_VARIANT_TYPE_STRING);

        if (actionValue != nullptr)
        {
          auto const storedAction = std::string_view{::g_variant_get_string(actionValue, nullptr)};
          ::g_variant_unref(actionValue);

          if (storedAction == action)
          {
            auto* const labelValue =
              ::g_menu_model_get_item_attribute_value(model, itemIndex, G_MENU_ATTRIBUTE_LABEL, G_VARIANT_TYPE_STRING);
            auto optResult = std::optional<std::string>{};

            if (labelValue != nullptr)
            {
              optResult = std::string{::g_variant_get_string(labelValue, nullptr)};
              ::g_variant_unref(labelValue);
            }

            return optResult;
          }
        }

        auto* const links = ::g_menu_model_iterate_item_links(model, itemIndex);
        [[maybe_unused]] char const* linkName = nullptr;
        GMenuModel* linkedModel = nullptr;

        while (::g_menu_link_iter_get_next(links, &linkName, &linkedModel) != 0)
        {
          auto const optResult = menuLabelForAction(linkedModel, action);
          ::g_object_unref(linkedModel);

          if (optResult)
          {
            ::g_object_unref(links);
            return optResult;
          }
        }

        ::g_object_unref(links);
      }

      return std::nullopt;
    }

    std::optional<std::string> menuLabelForAction(Gtk::PopoverMenu& popover, std::string_view const action)
    {
      return menuLabelForAction(::gtk_popover_menu_get_menu_model(popover.gobj()), action);
    }
  } // namespace

  TEST_CASE("TagEditController - routes submitted tag mutations", "[gtk][unit][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      {
        firstTrackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Controller Target 1"});
        secondTrackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Controller Target 2"});
      }};
    auto window = Gtk::Window{};

    auto themeCoordinator = ThemeCoordinator{};
    auto controller = TagEditController{window,
                                        fixture.runtime().async(),
                                        fixture.runtime().library(),
                                        fixture.runtime().completion(),
                                        fixture.runtime().notifications(),
                                        fixture.runtime().textOrderingPolicy(),
                                        ao::test::englishMessageCatalog(),
                                        {},
                                        themeCoordinator};

    SECTION("submitTagChanges publishes the mutated tracks through library changes")
    {
      auto mutatedIds = std::vector<TrackId>{};
      auto const changesSub = fixture.runtime().library().changes().onChanged(
        [&mutatedIds](rt::LibraryChangeSet const& changeSet)
        { mutatedIds.insert(mutatedIds.end(), changeSet.tracksMutated.begin(), changeSet.tracksMutated.end()); });

      auto const selection =
        TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {firstTrackId, secondTrackId}};

      controller.submitTagChanges(selection, {"ControllerTag"}, {});

      REQUIRE(pumpGtkEventsUntil([&mutatedIds] { return mutatedIds.size() == 2; }));
      CHECK(std::ranges::contains(mutatedIds, firstTrackId));
      CHECK(std::ranges::contains(mutatedIds, secondTrackId));
    }

    SECTION("submitTagChanges reports a concurrent retry as busy")
    {
      auto const selection =
        TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {firstTrackId, secondTrackId}};

      controller.submitTagChanges(selection, {"FirstConcurrentTag"}, {});
      controller.submitTagChanges(selection, {"SecondConcurrentTag"}, {});

      REQUIRE(pumpGtkEventsUntil(
        [&fixture]
        {
          auto const feed = fixture.runtime().notifications().feed();
          return std::ranges::any_of(feed.entries,
                                     [](auto const& entry)
                                     {
                                       auto const* message = std::get_if<std::string>(&entry.message);
                                       return entry.severity == rt::NotificationSeverity::Warning &&
                                              message != nullptr && *message == "Library is busy. Try again.";
                                     });
        }));
    }
  }

  TEST_CASE("TagEditController - tag popover attachment follows the anchor lifetime", "[gtk][regression][tag]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      { trackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Popover Target"}); }};
    auto window = Gtk::Window{};
    auto anchor = Gtk::Box{};
    window.set_child(anchor);
    window.present();
    drainGtkEvents();

    auto themeCoordinator = ThemeCoordinator{};
    auto controller = TagEditController{window,
                                        fixture.runtime().async(),
                                        fixture.runtime().library(),
                                        fixture.runtime().completion(),
                                        fixture.runtime().notifications(),
                                        fixture.runtime().textOrderingPolicy(),
                                        ao::test::englishMessageCatalog(),
                                        {},
                                        themeCoordinator};
    auto const selection = TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {trackId}};

    controller.openTagEditor(selection, anchor);
    REQUIRE(collectAll<Gtk::Popover>(anchor).size() == 1);

    controller.openTagEditor(selection, anchor);
    CHECK(collectAll<Gtk::Popover>(anchor).size() == 1);

    window.unset_child();
    drainGtkEvents();
    CHECK(collectAll<Gtk::Popover>(anchor).empty());

    window.set_child(anchor);
    window.present();
    drainGtkEvents();
    controller.openTagEditor(selection, anchor);
    CHECK(collectAll<Gtk::Popover>(anchor).size() == 1);
  }

  TEST_CASE("TagEditController - Edit Tags survives context popover close-before-action ordering",
            "[gtk][regression][tag][collation]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto policyRes = i18n::createIcuTextOrderingPolicy("de-DE");
    REQUIRE(policyRes);
    auto policyPtr = std::move(*policyRes);
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      {
        trackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Context Target"});
        library::test::addTrackWithUniqueFixtureUri(library, {.title = "Zulu Tag Source", .tags = {"z"}});
        library::test::addTrackWithUniqueFixtureUri(library, {.title = "Umlaut Tag Source", .tags = {"ä"}});
      },
      policyPtr.get()};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto modelPtr = TrackListModel::create(cache);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.async(),
                              runtime.library(),
                              runtime.views(),
                              thumbnailLoader};
    auto window = Gtk::Window{};
    window.set_child(page);
    window.present();
    drainGtkEvents();

    auto themeCoordinator = ThemeCoordinator{};
    auto controller = TagEditController{window,
                                        runtime.async(),
                                        runtime.library(),
                                        runtime.completion(),
                                        runtime.notifications(),
                                        runtime.textOrderingPolicy(),
                                        ao::test::englishMessageCatalog(),
                                        {},
                                        themeCoordinator};
    auto const selection = TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {trackId}};
    controller.openTrackContextMenu(page, selection, 20.0, 20.0);
    drainGtkEvents();

    auto const contextPopovers = collectAll<Gtk::PopoverMenu>(page);
    REQUIRE(contextPopovers.size() == 1);
    emitClosed(*contextPopovers.front());
    CHECK(contextPopovers.front()->activate_action("ctx.edit-tags"));
    drainGtkEvents();

    auto const popovers = collectAll<Gtk::Popover>(page);
    REQUIRE(popovers.size() == 1);
    CHECK(dynamic_cast<Gtk::PopoverMenu*>(popovers.front()) == nullptr);
    auto* const editor = findWidget<TagEditor>(*popovers.front());
    REQUIRE(editor != nullptr);
    CHECK(directChildLabelTextsByClass(*editor, "ao-tag-chip-suggested") == std::vector<std::string>{"ä", "z"});
  }

  TEST_CASE("TagEditController - empty Add to Playlist menu links to Playlist creation", "[gtk][unit][tag][playlist]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      { trackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Context Target"}); }};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto modelPtr = TrackListModel::create(cache);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.async(),
                              runtime.library(),
                              runtime.views(),
                              thumbnailLoader};
    auto window = Gtk::Window{};
    window.set_child(page);

    std::int32_t requestCount = 0;
    auto themeCoordinator = ThemeCoordinator{};
    auto controller =
      TagEditController{window,
                        runtime.async(),
                        runtime.library(),
                        runtime.completion(),
                        runtime.notifications(),
                        runtime.textOrderingPolicy(),
                        ao::test::englishMessageCatalog(),
                        TagEditController::Callbacks{.onManageListsRequested = [&requestCount] { ++requestCount; }},
                        themeCoordinator};
    controller.openTrackContextMenu(
      page, TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {trackId}}, 20.0, 20.0);

    auto const contextPopovers = collectAll<Gtk::PopoverMenu>(page);
    REQUIRE(contextPopovers.size() == 1);
    CHECK(contextPopovers.front()->activate_action("ctx.manage-lists"));
    CHECK(requestCount == 1);
    drainGtkEvents();
  }

  TEST_CASE("TagEditController - Playlist membership menu distinguishes writable and computed Lists",
            "[gtk][unit][tag][playlist]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      { trackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Context Target"}); }};
    auto& runtime = fixture.runtime();
    auto const writableId = ao::test::requireValue(runGtkTask(
      runtime, runtime.library().commands().createList(rt::ListDraft{.name = "Road Trip", .expression = "#roadtrip"})));
    auto const computedId = ao::test::requireValue(runGtkTask(
      runtime,
      runtime.library().commands().createList(rt::ListDraft{.name = "Recent", .expression = "$year >= 2020"})));
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto modelPtr = TrackListModel::create(cache);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto allTracksPage = TrackViewPage{rt::kAllTracksListId,
                                       modelPtr,
                                       columnLayouts,
                                       ao::test::englishMessageCatalog(),
                                       runtime.async(),
                                       runtime.library(),
                                       runtime.views(),
                                       thumbnailLoader};
    auto window = Gtk::Window{};
    window.set_child(allTracksPage);
    window.present();
    drainGtkEvents();
    std::int32_t requestCount = 0;
    auto themeCoordinator = ThemeCoordinator{};
    auto controller =
      TagEditController{window,
                        runtime.async(),
                        runtime.library(),
                        runtime.completion(),
                        runtime.notifications(),
                        runtime.textOrderingPolicy(),
                        ao::test::englishMessageCatalog(),
                        TagEditController::Callbacks{.onManageListsRequested = [&requestCount] { ++requestCount; }},
                        themeCoordinator};

    controller.openTrackContextMenu(
      allTracksPage, TrackSelection{.listId = rt::kAllTracksListId, .selectedIds = {trackId}}, 20.0, 20.0);

    auto contextPopovers = collectAll<Gtk::PopoverMenu>(allTracksPage);
    REQUIRE(contextPopovers.size() == 1);
    auto* const allTracksMenu = contextPopovers.front();
    CHECK(menuLabelForAction(*allTracksMenu, std::format("ctx.add-to-list-{}", writableId.raw())) ==
          std::optional<std::string>{"Road Trip (#roadtrip)"});
    CHECK_FALSE(menuLabelForAction(*allTracksMenu, std::format("ctx.add-to-list-{}", computedId.raw())));
    CHECK(
      menuLabelForAction(*allTracksMenu, "ctx.computed-lists-omitted") ==
      std::optional<std::string>{"Other Lists have computed membership; edit their expression or track tags instead."});
    CHECK(menuLabelForAction(*allTracksMenu, "ctx.manage-lists") == std::optional<std::string>{"Manage Lists..."});
    CHECK(allTracksMenu->activate_action("ctx.manage-lists"));
    CHECK(requestCount == 1);
    drainGtkEvents();

    auto computedPage = TrackViewPage{computedId,
                                      modelPtr,
                                      columnLayouts,
                                      ao::test::englishMessageCatalog(),
                                      runtime.async(),
                                      runtime.library(),
                                      runtime.views(),
                                      thumbnailLoader};
    window.unset_child();
    window.set_child(computedPage);
    window.present();
    drainGtkEvents();
    controller.openTrackContextMenu(
      computedPage, TrackSelection{.listId = computedId, .selectedIds = {trackId}}, 20.0, 20.0);

    contextPopovers = collectAll<Gtk::PopoverMenu>(computedPage);
    REQUIRE(contextPopovers.size() == 1);
    CHECK(menuLabelForAction(*contextPopovers.front(), "ctx.remove-from-current-list-unavailable") ==
          std::optional<std::string>{"Remove from this List is unavailable because its membership is computed."});
    CHECK_FALSE(menuLabelForAction(*contextPopovers.front(), "ctx.remove-from-current-list"));
  }

  TEST_CASE("TagEditController - quick filter menu keeps absolute Manual Order actions", "[gtk][unit][tag][list-order]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& library)
      { trackId = library::test::addTrackWithUniqueFixtureUri(library, {.title = "Context Target"}); }};
    auto& runtime = fixture.runtime();
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createList(rt::ListDraft{.name = "Ordered"})));
    auto const* manual = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
    REQUIRE(manual != nullptr);
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate(rt::NavigationRequest{
      .target = rt::FilteredListTarget{.listId = listId, .filterExpression = "$year >= 2020"},
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = manual->spec,
        },
    }));
    auto projectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(cache);
    modelPtr->bindProjection(projectionPtr);
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto page = TrackViewPage{listId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.async(),
                              runtime.library(),
                              runtime.views(),
                              thumbnailLoader,
                              manual->spec,
                              viewId};
    auto const capabilities = page.orderCapabilities();

    CHECK(capabilities.canAuthorOrder);
    CHECK_FALSE(capabilities.canRelativeMove);
    CHECK(capabilities.canAbsoluteMove);
    CHECK(capabilities.canResetOrder);
    CHECK(capabilities.canForgetHiddenPositions);
    auto* const status = findLabelByText(
      page, "Clear the quick filter to drag or move relatively; moving to the top or bottom is still available.");
    REQUIRE(status != nullptr);
    CHECK(status->get_visible());

    auto window = Gtk::Window{};
    window.set_child(page);
    window.present();
    drainGtkEvents();
    auto themeCoordinator = ThemeCoordinator{};
    auto controller = TagEditController{window,
                                        runtime.async(),
                                        runtime.library(),
                                        runtime.completion(),
                                        runtime.notifications(),
                                        runtime.textOrderingPolicy(),
                                        ao::test::englishMessageCatalog(),
                                        {},
                                        themeCoordinator};
    controller.openTrackContextMenu(page, TrackSelection{.listId = listId, .selectedIds = {trackId}}, 20.0, 20.0);

    auto const contextPopovers = collectAll<Gtk::PopoverMenu>(page);
    REQUIRE(contextPopovers.size() == 1);
    auto& menu = *contextPopovers.front();
    CHECK(menuLabelForAction(menu, "ctx.order-up") == std::optional<std::string>{"Move Up"});
    CHECK(menuLabelForAction(menu, "ctx.order-down") == std::optional<std::string>{"Move Down"});
    CHECK(menuLabelForAction(menu, "ctx.relative-ordering-unavailable") ==
          std::optional<std::string>{
            "Clear the quick filter to drag or move relatively; moving to the top or bottom is still available."});
    CHECK(menuLabelForAction(menu, "ctx.order-top") == std::optional<std::string>{"Move to Top"});
    CHECK(menuLabelForAction(menu, "ctx.order-bottom") == std::optional<std::string>{"Move to Bottom"});
    CHECK(menuLabelForAction(menu, "ctx.order-reset") == std::optional<std::string>{"Reset Order"});
    CHECK(menuLabelForAction(menu, "ctx.order-forget-hidden") == std::optional<std::string>{"Forget Hidden Positions"});
  }
} // namespace ao::gtk::test
