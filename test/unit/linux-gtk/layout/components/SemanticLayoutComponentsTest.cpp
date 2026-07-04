// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ThemeCoordinator.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/track/TrackRowCache.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "track/TrackPageHost.h"
#include "track/TrackQuickFilter.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/projection/ProjectionTypes.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/playback/queue/PlaybackQueueModel.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/menu.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::collectAll;
  using ao::gtk::test::drainGtkEvents;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findButtonByLabel;
  using ao::gtk::test::findWidget;
  using ao::gtk::test::findWidgetByClass;
  using ao::gtk::test::measureWidget;

  namespace
  {
    library::test::TrackSpec trackSpecFor(library::MusicLibrary& musicLibrary, TrackId const trackId)
    {
      auto const txn = musicLibrary.readTransaction();
      auto const optView = musicLibrary.tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      return library::test::trackSpecFromView(musicLibrary, *optView);
    }
  } // namespace

  TEST_CASE("Semantic layout components render missing-service errors", "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();

    SECTION("library.listTree shows error when rowDataProvider missing")
    {
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("trackRowCache missing") != std::string::npos);
    }

    SECTION("library.listTree shows error when listNavigationController missing")
    {
      auto const rdpPtr = std::make_unique<TrackRowCache>(fixture.runtime().library());
      ctx.track.trackRowCache = rdpPtr.get();
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("listNavigationController missing") != std::string::npos);
    }

    SECTION("tracks.table shows error when trackPageGraph missing")
    {
      auto const node = LayoutNode{.type = "tracks.table"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);

      auto* const child = box->get_first_child();
      REQUIRE(child != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(child);
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("trackPageHost missing") != std::string::npos);
    }
  }

  TEST_CASE("Semantic layout components render configured GTK widgets", "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();

    int const cacheSize = 10;
    auto imageCachePtr = std::make_unique<ImageCache>(cacheSize);
    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append_submenu("Test Menu", Gio::Menu::create());
    ctx.detail.imageCache = imageCachePtr.get();
    ctx.shell.menuModelPtr = menuModelPtr;

    {
      auto const node = LayoutNode{.type = "status.messageLabel"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Aobus Ready");
    }

    SECTION("library.openLibraryButton creates Gtk::Button")
    {
      auto const node = LayoutNode{.type = "library.openLibraryButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "folder-open-symbolic");
    }

    SECTION("app.menuBar creates Gtk::PopoverMenuBar")
    {
      auto const node = LayoutNode{.type = "app.menuBar"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const menuBar = dynamic_cast<Gtk::PopoverMenuBar*>(&compPtr->widget());
      CHECK(menuBar != nullptr);
    }

    SECTION("app.menuButton creates Gtk::MenuButton and sets menu model")
    {
      auto const node = LayoutNode{.type = "app.menuButton", .props = {{"icon", LayoutValue{"test-icon"}}}};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const menuButton = dynamic_cast<Gtk::MenuButton*>(&compPtr->widget());
      REQUIRE(menuButton != nullptr);
      CHECK(menuButton->get_icon_name() == "test-icon");
      CHECK(menuButton->get_menu_model() == menuModelPtr);
    }

    SECTION("app.menuBar tolerates absent menu model")
    {
      ctx.shell.menuModelPtr.reset();
      auto const node = LayoutNode{.type = "app.menuBar"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::PopoverMenuBar*>(&compPtr->widget()) != nullptr);
    }

    SECTION("track.detailScope creates box and acts as scope provider")
    {
      auto const node = LayoutNode{.type = "track.detailScope"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Box*>(&compPtr->widget()) != nullptr);
      CHECK(ctx.track.detailScope == nullptr); // Ensure context is restored
    }

    SECTION("track.selectionRegion creates box container")
    {
      auto const node = LayoutNode{.type = "track.selectionRegion"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Box*>(&compPtr->widget()) != nullptr);
    }

    SECTION("track.selectionRegion can retain a disabled no-selection placeholder")
    {
      auto& scope = fixture.attachTrackDetailScope();

      auto node = LayoutNode{.type = "track.selectionRegion",
                             .props = {{"showPlaceholder", LayoutValue{true}}},
                             .children = {LayoutNode{.type = "spacer"}}};
      auto const compPtr = fixture.create(node);
      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      CHECK(widget.get_visible());
      CHECK_FALSE(widget.get_sensitive());

      auto selected = rt::TrackDetailSnapshot{};
      selected.selectionKind = rt::SelectionKind::Single;
      selected.trackIds = {TrackId{1}};
      scope.setSnapshot(std::move(selected));

      CHECK(widget.get_visible());
      CHECK(widget.get_sensitive());
    }

    SECTION("track.coverArt creates a stable responsive square slot")
    {
      auto node = LayoutNode{.type = "track.coverArt"};
      node.props["targetSize"] = LayoutValue{static_cast<std::int64_t>(250)};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      CHECK(widget.get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(widget.get_first_child() != nullptr);

      auto const horizontalMeasure = measureWidget(widget, Gtk::Orientation::HORIZONTAL, -1);
      CHECK(horizontalMeasure.minimum == 0);
      CHECK(horizontalMeasure.natural == 250);

      auto const heightConstrainedHorizontalMeasure = measureWidget(widget, Gtk::Orientation::HORIZONTAL, 233);
      CHECK(heightConstrainedHorizontalMeasure.minimum == 0);
      CHECK(heightConstrainedHorizontalMeasure.natural == 233);

      auto const unconstrainedVerticalMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, -1);
      CHECK(unconstrainedVerticalMeasure.minimum == 0);
      CHECK(unconstrainedVerticalMeasure.natural == 250);

      auto const narrowVerticalMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, 180);
      CHECK(narrowVerticalMeasure.minimum == 0);
      CHECK(narrowVerticalMeasure.natural == 180);

      auto const wideVerticalMeasure = measureWidget(widget, Gtk::Orientation::VERTICAL, 320);
      CHECK(wideVerticalMeasure.minimum == 0);
      CHECK(wideVerticalMeasure.natural == 250);

      auto* const imageWidget = widget.get_first_child();
      CHECK(imageWidget != nullptr);
      fixture.window().set_child(widget);

      widget.size_allocate(Gtk::Allocation{0, 0, 180, 300}, -1);
      CHECK(widget.get_width() == 180);

      fixture.window().unset_child();
    }

    SECTION("track.fieldGrid creates grid and acts as scope subscriber")
    {
      auto const node = LayoutNode{.type = "track.fieldGrid"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      auto& root = compPtr->widget();
      auto* const grid = findWidget<Gtk::Grid>(root);
      CHECK(grid != nullptr);
      CHECK(dynamic_cast<Gtk::ScrolledWindow*>(&root) == nullptr);
      CHECK(dynamic_cast<Gtk::ScrolledWindow*>(grid != nullptr ? grid->get_parent() : nullptr) == nullptr);
    }

    SECTION("track.detailUndoBar reflects pending custom metadata undo")
    {
      auto undoController = TrackDetailUndoController{fixture.runtime().library().writer()};
      ctx.track.detailUndo = &undoController;

      auto const node = LayoutNode{.type = "track.detailUndoBar"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      auto& bar = compPtr->widget();
      CHECK_FALSE(bar.get_visible());

      undoController.showCustomMetadataDeleted("Mood", {TrackId{1}}, "Energetic");
      drainGtkEvents();

      CHECK(bar.get_visible());
      auto* const label = findWidget<Gtk::Label>(bar);
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Custom metadata 'Mood' removed");

      undoController.clear();
      drainGtkEvents();

      CHECK_FALSE(bar.get_visible());
    }

    SECTION("track.tagEditor creates tag editor container")
    {
      auto const node = LayoutNode{.type = "track.tagEditor"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(!compPtr->widget().get_name().empty());
    }

    SECTION("track.tagEditor keeps its empty-selection footprint")
    {
      fixture.attachTrackDetailScope();

      auto const node = LayoutNode{.type = "track.tagEditor"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(compPtr->widget().get_visible());
    }
  }

  TEST_CASE("TrackDetailUndoController restores deleted custom metadata", "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_undo_test"};
    auto& musicLibrary = fixture.runtime().musicLibrary();
    auto const trackId = library::test::addTrack(musicLibrary, {.title = "Undo Target"});
    auto undoController = TrackDetailUndoController{fixture.runtime().library().writer()};

    undoController.showCustomMetadataDeleted("Mood", {trackId}, "Bright");
    undoController.undo();

    auto const txn = musicLibrary.readTransaction();
    auto const optView = musicLibrary.tracks().reader(txn).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);

    auto const spec = library::test::trackSpecFromView(musicLibrary, *optView);
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata[0].first == "Mood");
    CHECK(spec.customMetadata[0].second == "Bright");
  }

  TEST_CASE("TrackDetailUndoController clears pending undo after timeout", "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_undo_timeout_test"};
    auto timeoutCallback = sigc::slot<bool()>{};
    auto controller = TrackDetailUndoController{fixture.runtime().library().writer(),
                                                [&](std::chrono::milliseconds interval, sigc::slot<bool()> callback)
                                                {
                                                  CHECK(interval == std::chrono::milliseconds{5000});
                                                  timeoutCallback = std::move(callback);
                                                  return sigc::connection{};
                                                }};

    controller.showCustomMetadataDeleted("Mood", {TrackId{1}}, "Bright");
    REQUIRE(controller.pendingCustomMetadataUndo());
    REQUIRE(!timeoutCallback.empty());

    CHECK(timeoutCallback() == false);

    CHECK_FALSE(controller.pendingCustomMetadataUndo());
  }

  TEST_CASE("TrackDetailScope clears pending detail undo when selection changes",
            "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_undo_scope_test"};
    auto& runtime = fixture.runtime();
    auto& musicLibrary = runtime.musicLibrary();
    auto const firstTrackId =
      library::test::addTrack(musicLibrary, {.title = "First", .customMetadata = {{"Mood", "Bright"}}});
    auto const secondTrackId = library::test::addTrack(musicLibrary, {.title = "Second"});

    auto const reply = runtime.views().createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
    runtime.workspace().setFocusedView(reply.viewId);
    runtime.views().setSelection(reply.viewId, {firstTrackId});
    drainGtkEvents();

    auto const node =
      LayoutNode{.type = "track.detailScope",
                 .children = {LayoutNode{.type = "track.fieldGrid"}, LayoutNode{.type = "track.detailUndoBar"}}};
    auto const compPtr = fixture.create(node);
    REQUIRE(compPtr != nullptr);

    auto& root = compPtr->widget();
    auto* const undoBar = findWidgetByClass<Gtk::Widget>(root, "ao-undo-bar");
    REQUIRE(undoBar != nullptr);
    CHECK_FALSE(undoBar->get_visible());

    auto* const deleteButton = findWidgetByClass<Gtk::Button>(root, "ao-detail-field-delete");
    REQUIRE(deleteButton != nullptr);
    emitClicked(*deleteButton);
    drainGtkEvents();

    CHECK(undoBar->get_visible());

    runtime.views().setSelection(reply.viewId, {secondTrackId});
    drainGtkEvents();

    CHECK_FALSE(undoBar->get_visible());
  }

  TEST_CASE("TrackDetailUndoBar restores deleted custom metadata from button",
            "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_undo_button_test"};
    auto& runtime = fixture.runtime();
    auto& musicLibrary = runtime.musicLibrary();
    auto const trackId =
      library::test::addTrack(musicLibrary, {.title = "Undo Button Target", .customMetadata = {{"Mood", "Bright"}}});

    auto const reply = runtime.views().createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
    runtime.workspace().setFocusedView(reply.viewId);
    runtime.views().setSelection(reply.viewId, {trackId});
    drainGtkEvents();

    auto const node =
      LayoutNode{.type = "track.detailScope",
                 .children = {LayoutNode{.type = "track.fieldGrid"}, LayoutNode{.type = "track.detailUndoBar"}}};
    auto const compPtr = fixture.create(node);
    REQUIRE(compPtr != nullptr);

    auto& root = compPtr->widget();
    auto* const undoBar = findWidgetByClass<Gtk::Widget>(root, "ao-undo-bar");
    REQUIRE(undoBar != nullptr);

    auto* const deleteButton = findWidgetByClass<Gtk::Button>(root, "ao-detail-field-delete");
    REQUIRE(deleteButton != nullptr);
    emitClicked(*deleteButton);
    drainGtkEvents();

    CHECK(trackSpecFor(musicLibrary, trackId).customMetadata.empty());
    CHECK(undoBar->get_visible());

    auto* const undoButton = findWidgetByClass<Gtk::Button>(root, "ao-undo-button");
    REQUIRE(undoButton != nullptr);
    emitClicked(*undoButton);
    drainGtkEvents();

    auto const spec = trackSpecFor(musicLibrary, trackId);
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata[0].first == "Mood");
    CHECK(spec.customMetadata[0].second == "Bright");
    CHECK_FALSE(undoBar->get_visible());
  }

  TEST_CASE("TrackFieldGrid add custom metadata writes metadata and clears stale delete undo",
            "[gtk][unit][layout][component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_add_custom_test"};
    auto& runtime = fixture.runtime();
    auto& musicLibrary = runtime.musicLibrary();
    auto const trackId =
      library::test::addTrack(musicLibrary, {.title = "Add Target", .customMetadata = {{"Mood", "Bright"}}});

    auto const reply = runtime.views().createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
    runtime.workspace().setFocusedView(reply.viewId);
    runtime.views().setSelection(reply.viewId, {trackId});
    drainGtkEvents();

    auto const node =
      LayoutNode{.type = "track.detailScope",
                 .children = {LayoutNode{.type = "track.fieldGrid"}, LayoutNode{.type = "track.detailUndoBar"}}};
    auto const compPtr = fixture.create(node);
    REQUIRE(compPtr != nullptr);

    auto& root = compPtr->widget();
    fixture.window().set_child(root);

    auto* const deleteButton = findWidgetByClass<Gtk::Button>(root, "ao-detail-field-delete");
    REQUIRE(deleteButton != nullptr);
    emitClicked(*deleteButton);
    drainGtkEvents();

    auto* const undoBar = findWidgetByClass<Gtk::Widget>(root, "ao-undo-bar");
    REQUIRE(undoBar != nullptr);
    CHECK(undoBar->get_visible());
    CHECK(trackSpecFor(musicLibrary, trackId).customMetadata.empty());

    auto* const addButton = findWidgetByClass<Gtk::Button>(root, "ao-detail-add-custom-metadata-button");
    REQUIRE(addButton != nullptr);
    emitClicked(*addButton);
    drainGtkEvents();

    auto* const popover = findWidget<Gtk::Popover>(*addButton);
    REQUIRE(popover != nullptr);
    auto entries = collectAll<Gtk::Entry>(*popover);
    REQUIRE(entries.size() == 2);
    entries[0]->set_text("Mood");
    entries[1]->set_text("Dark");

    auto* const submitButton = findButtonByLabel(*popover, "Add");
    REQUIRE(submitButton != nullptr);
    emitClicked(*submitButton);
    drainGtkEvents();

    auto const spec = trackSpecFor(musicLibrary, trackId);
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata[0].first == "Mood");
    CHECK(spec.customMetadata[0].second == "Dark");
    CHECK_FALSE(undoBar->get_visible());
    CHECK_FALSE(popover->get_visible());

    fixture.window().unset_child();
  }

  TEST_CASE("track.quickFilter component wires create smart list action", "[gtk][unit][layout][component][semantic]")
  {
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{runtime.library()};
    auto window = Gtk::Window{};
    auto stack = Gtk::Stack{};
    auto themeController = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{window, runtime, std::move(tagEditCallbacks), themeController};
    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{window, runtime, std::move(navCallbacks), themeController};
    auto layoutStore = uimodel::TrackColumnLayoutStore{};
    auto queueModel = uimodel::PlaybackQueueModel{runtime.playback()};
    auto pageHost = TrackPageHost{stack, runtime, &queueModel, tagEditController, listNavigation, layoutStore};

    runtime.workspace().navigateTo(rt::kAllTracksListId);
    drainGtkEvents();

    auto txn = library.readTransaction();
    pageHost.rebuild(cache, txn);
    drainGtkEvents();

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};
    ctx.track.pageHost = &pageHost;
    auto pendingDebounce = sigc::slot<bool()>{};
    ctx.timeoutScheduler = [&](std::chrono::milliseconds interval, sigc::slot<bool()> callback)
    {
      CHECK(interval == std::chrono::milliseconds{200});
      pendingDebounce = std::move(callback);
      return sigc::connection{};
    };
    auto capturedParentId = kInvalidListId;
    auto capturedExpression = std::string{};
    ctx.list.createSmartListFromExpression = [&](ListId parentListId, std::string expression)
    {
      capturedParentId = parentListId;
      capturedExpression = std::move(expression);
    };

    auto const node = LayoutNode{.type = "track.quickFilter"};
    auto const compPtr = registry.create(ctx, node);
    REQUIRE(compPtr != nullptr);

    auto* const filter = dynamic_cast<TrackQuickFilter*>(&compPtr->widget());
    REQUIRE(filter != nullptr);

    filter->setText(R"($artist = "Muse")");
    REQUIRE(!pendingDebounce.empty());
    CHECK(pendingDebounce() == false);
    drainGtkEvents();

    auto* const createButton = findWidgetByClass<Gtk::Button>(*filter, "ao-quick-filter-create");
    REQUIRE(createButton != nullptr);

    emitClicked(*createButton);
    drainGtkEvents();

    CHECK(capturedParentId == kInvalidListId);
    CHECK(capturedExpression == R"($artist = "Muse")");
  }
} // namespace ao::gtk::layout::test
