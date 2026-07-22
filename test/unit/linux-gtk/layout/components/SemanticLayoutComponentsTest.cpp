// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ThemeCoordinator.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntimeState.h"
#include "app/linux-gtk/track/TrackRowCache.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "tag/TagEditor.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "track/TrackPageHost.h"
#include "track/TrackQuickFilter.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>

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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
    constexpr std::size_t kOversizedMetadataLength = std::size_t{1024} * 1024;

    library::test::TrackSpec trackSpecFor(library::MusicLibrary const& musicLibrary, TrackId const trackId)
    {
      auto const transaction = musicLibrary.readTransaction();
      auto const optView =
        musicLibrary.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      return library::test::trackSpecFromView(musicLibrary, *optView);
    }
  } // namespace

  TEST_CASE("SemanticLayoutComponents - render missing-service errors", "[gtk][unit][layout-component][semantic]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("library.listTree shows error when rowDataProvider missing")
    {
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().raw().contains("trackRowCache missing"));
    }

    SECTION("library.listTree shows error when listNavigationController missing")
    {
      auto const rdpPtr = std::make_unique<TrackRowCache>(fixture.runtime().library());
      fixture.dependencies().trackRowCache = rdpPtr.get();
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().raw().contains("listNavigationController missing"));
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
      CHECK(label->get_label().raw().contains("trackPageHost missing"));
    }

    SECTION("track.coverArt shows error when imageCache missing")
    {
      auto const compPtr = fixture.create(LayoutNode{.type = "track.coverArt"});

      REQUIRE(compPtr != nullptr);
      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().raw().contains("imageCache missing"));
    }
  }

  TEST_CASE("SemanticLayoutComponents - render configured GTK widgets", "[gtk][unit][layout-component][semantic]")
  {
    auto undoTrackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.layout_test",
      [&undoTrackId](library::MusicLibrary& musicLibrary)
      { undoTrackId = library::test::addTrack(musicLibrary, {.title = "Undo notification target"}); }};
    auto& ctx = fixture.context();

    int const cacheSize = 10;
    auto imageCachePtr = std::make_unique<ImageCache>(cacheSize);
    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append_submenu("Test Menu", Gio::Menu::create());
    fixture.dependencies().imageCache = imageCachePtr.get();
    fixture.dependencies().menuModelPtr = menuModelPtr;

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
      fixture.dependencies().menuModelPtr.reset();
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
      CHECK(ctx.detailScope == nullptr); // Ensure context is restored
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
      auto undoController = TrackDetailUndoController{};
      ctx.detailUndo = &undoController;

      auto const node = LayoutNode{.type = "track.detailUndoBar"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      auto& bar = compPtr->widget();
      CHECK_FALSE(bar.get_visible());

      auto sessionPtr =
        ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{undoTrackId}));
      undoController.presentCustomMetadataDeletedUndo("Mood", "Energetic", std::move(sessionPtr));
      drainGtkEvents();

      CHECK(bar.get_visible());
      auto* const label = findWidget<Gtk::Label>(bar);
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Custom metadata 'Mood' removed");

      undoController.clear();
      drainGtkEvents();

      CHECK_FALSE(bar.get_visible());

      auto rejectedSessionPtr =
        ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{undoTrackId}));
      undoController.presentCustomMetadataDeletedUndo(
        "Mood", std::string(kOversizedMetadataLength, 'x'), std::move(rejectedSessionPtr));
      auto* const undoButton = findWidgetByClass<Gtk::Button>(bar, "ao-undo-button");
      REQUIRE(undoButton != nullptr);
      emitClicked(*undoButton);
      drainGtkEvents();

      auto const feed = fixture.runtime().notifications().feed();
      REQUIRE_FALSE(feed.entries.empty());
      CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);
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

  TEST_CASE("TrackTagEditorComponent - snapshot callbacks outlive the transient build context",
            "[gtk][regression][layout-component]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& scope = fixture.attachTrackDetailScope();
    auto const componentPtr = fixture.createWithTransientContext(LayoutNode{.type = "track.tagEditor"});
    REQUIRE(componentPtr != nullptr);

    auto snapshot = rt::TrackDetailSnapshot{};
    snapshot.trackIds = {TrackId{123}};
    scope.setSnapshot(snapshot);

    CHECK(scope.snapshot().trackIds == snapshot.trackIds);
    CHECK(componentPtr->widget().get_visible());
  }

  TEST_CASE("TrackTagEditorComponent - fallback reports a stale tag submission",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.tag_editor_stale_fallback_test",
                                        [&trackId](library::MusicLibrary& musicLibrary)
                                        { trackId = library::test::addTrack(musicLibrary, {.title = "Tag Target"}); }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope();
    auto snapshot = rt::TrackDetailSnapshot{};
    snapshot.selectionKind = rt::SelectionKind::Single;
    snapshot.trackIds = {trackId};
    scope.setSnapshot(std::move(snapshot));

    auto const componentPtr = fixture.create(LayoutNode{.type = "track.tagEditor"});
    REQUIRE(componentPtr != nullptr);
    auto* const editor = dynamic_cast<TagEditor*>(&componentPtr->widget());
    REQUIRE(editor != nullptr);

    auto const firstAddition = std::array{std::string{"First"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{firstAddition}, std::span<std::string const>{});

    auto const expectedTags = std::vector<std::string>{"First"};
    CHECK(trackSpecFor(runtime.musicLibrary(), trackId).tags == expectedTags);
    auto feed = runtime.notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
    CHECK(std::get<std::string>(feed.entries.back().message) == "Tags added 1 for 1 track");

    REQUIRE(runtime.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));
    auto const secondAddition = std::array{std::string{"Second"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{secondAddition}, std::span<std::string const>{});

    CHECK(trackSpecFor(runtime.musicLibrary(), trackId).tags == expectedTags);
    feed = runtime.notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);
    CHECK(std::get<std::string>(feed.entries.back().message) ==
          "Library changed while the tag editor was open. Reload and try again.");
  }

  TEST_CASE("TrackDetailUndoController - restores deleted custom metadata", "[gtk][unit][layout-component][semantic]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_undo_test",
                                        [&trackId](library::MusicLibrary& musicLibrary)
                                        { trackId = library::test::addTrack(musicLibrary, {.title = "Undo Target"}); }};
    auto const& musicLibrary = fixture.runtime().musicLibrary();
    auto undoController = TrackDetailUndoController{};
    auto sessionPtr =
      ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{trackId}));

    undoController.presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(sessionPtr));
    REQUIRE(undoController.undo());

    auto const transaction = musicLibrary.readTransaction();
    auto const optView =
      musicLibrary.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);

    auto const spec = library::test::trackSpecFromView(musicLibrary, *optView);
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata[0].first == "Mood");
    CHECK(spec.customMetadata[0].second == "Bright");
  }

  TEST_CASE("TrackDetailUndoController - clears pending undo after timeout", "[gtk][unit][layout-component][semantic]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.detail_undo_timeout_test",
                           [&trackId](library::MusicLibrary& musicLibrary)
                           { trackId = library::test::addTrack(musicLibrary, {.title = "Undo timeout target"}); }};
    auto timeoutCallback = sigc::slot<bool()>{};
    auto controller = TrackDetailUndoController{[&](std::chrono::milliseconds interval, sigc::slot<bool()> callback)
                                                {
                                                  CHECK(interval == std::chrono::milliseconds{5000});
                                                  timeoutCallback = std::move(callback);
                                                  return sigc::connection{};
                                                }};
    auto sessionPtr =
      ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{trackId}));

    controller.presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(sessionPtr));
    REQUIRE(controller.pendingCustomMetadataUndo());
    REQUIRE(!timeoutCallback.empty());

    CHECK(timeoutCallback() == false);

    CHECK_FALSE(controller.pendingCustomMetadataUndo());
  }

  TEST_CASE("TrackDetailUndoController - an intervening commit makes undo stale",
            "[gtk][unit][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_stale_undo_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrack(musicLibrary, {.customMetadata = {{"Mood", "Bright"}}}); }};
    auto& runtime = fixture.runtime();
    auto sessionPtr = ao::test::requireValue(TrackAuthoringSession::begin(runtime.library(), std::array{trackId}));
    auto deletePatch = rt::MetadataPatch{};
    deletePatch.customUpdates["Mood"] = std::nullopt;
    auto deleteResult = sessionPtr->submitMetadata(deletePatch);
    REQUIRE(deleteResult);
    REQUIRE(deleteResult->status == rt::TrackAuthoringStatus::Applied);
    auto controller = TrackDetailUndoController{};
    controller.presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(sessionPtr));

    REQUIRE(runtime.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));
    REQUIRE(controller.pendingCustomMetadataUndo());
    CHECK_FALSE(controller.pendingCustomMetadataUndo()->sessionPtr->isCurrent());

    auto const undoResult = controller.undo();

    REQUIRE_FALSE(undoResult);
    CHECK(undoResult.error().message == "Library changed before metadata undo could be applied");
    CHECK_FALSE(controller.pendingCustomMetadataUndo());
    CHECK(trackSpecFor(runtime.musicLibrary(), trackId).customMetadata.empty());
  }

  TEST_CASE("TrackDetailUndoController - rejected undo clears the terminal action",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.detail_rejected_undo_test",
                           [&trackId](library::MusicLibrary& musicLibrary)
                           { trackId = library::test::addTrack(musicLibrary, {.title = "Rejected undo target"}); }};
    auto sessionPtr =
      ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{trackId}));
    auto controller = TrackDetailUndoController{};
    std::size_t changedCount = 0;
    auto changedConnection = controller.signalChanged().connect([&changedCount] { ++changedCount; });

    controller.presentCustomMetadataDeletedUndo(
      "Mood", std::string(kOversizedMetadataLength, 'x'), std::move(sessionPtr));
    REQUIRE(controller.pendingCustomMetadataUndo());

    auto const undoResult = controller.undo();

    REQUIRE_FALSE(undoResult);
    CHECK_FALSE(controller.pendingCustomMetadataUndo());
    CHECK(changedCount == 2);
    CHECK(trackSpecFor(fixture.runtime().musicLibrary(), trackId).customMetadata.empty());
  }

  TEST_CASE("TrackFieldGrid - a stale authoring session cancels the active editor",
            "[gtk][unit][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_stale_editor_test",
                                        [&trackId](library::MusicLibrary& musicLibrary)
                                        { trackId = library::test::addTrack(musicLibrary, {.title = "Before"}); }};
    auto& runtime = fixture.runtime();
    auto const navigation = ao::test::requireValue(runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks));
    REQUIRE(runtime.views().setSelection(navigation, {trackId}));
    drainGtkEvents();

    auto const componentPtr =
      fixture.create(LayoutNode{.type = "track.detailScope", .children = {LayoutNode{.type = "track.fieldGrid"}}});
    REQUIRE(componentPtr != nullptr);
    auto& root = componentPtr->widget();
    fixture.window().set_child(root);
    auto const editors = collectAll<track_field_grid::DetailFieldEditor>(root);
    auto const titleEditorIter =
      std::ranges::find_if(editors, [](auto const* editor) { return editor->text().raw() == "Before"; });
    REQUIRE(titleEditorIter != editors.end());
    auto* const titleEditor = *titleEditorIter;

    emitClicked(titleEditor->editButton());
    REQUIRE(titleEditor->isEditing());
    REQUIRE(runtime.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));
    drainGtkEvents();

    CHECK_FALSE(titleEditor->isEditing());
    CHECK(titleEditor->text().raw() == "Before");
    fixture.window().unset_child();
  }

  TEST_CASE("TrackFieldGrid - stale custom metadata commit is reported without changing storage",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_stale_custom_metadata_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrack(musicLibrary, {.customMetadata = {{"Mood", "Bright"}}}); }};
    auto& runtime = fixture.runtime();
    auto const navigation = ao::test::requireValue(runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks));
    REQUIRE(runtime.views().setSelection(navigation, {trackId}));
    drainGtkEvents();

    auto const componentPtr =
      fixture.create(LayoutNode{.type = "track.detailScope", .children = {LayoutNode{.type = "track.fieldGrid"}}});
    REQUIRE(componentPtr != nullptr);
    auto& root = componentPtr->widget();
    fixture.window().set_child(root);
    auto const editors = collectAll<track_field_grid::DetailFieldEditor>(root);
    auto const moodEditorIter =
      std::ranges::find_if(editors, [](auto const* editor) { return editor->text().raw() == "Bright"; });
    REQUIRE(moodEditorIter != editors.end());
    auto* const moodEditor = *moodEditorIter;

    moodEditor->startEditing();
    REQUIRE(moodEditor->isEditing());
    moodEditor->entry().set_text("Dark");
    REQUIRE(runtime.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));
    moodEditor->stopEditing(true);

    auto const spec = trackSpecFor(runtime.musicLibrary(), trackId);
    REQUIRE(spec.customMetadata.size() == 1);
    auto const expectedMetadata = std::pair{std::string{"Mood"}, std::string{"Bright"}};
    CHECK(spec.customMetadata.front() == expectedMetadata);
    auto const feed = runtime.notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);
    CHECK(std::get<std::string>(feed.entries.back().message) ==
          "Library changed while this edit was open. Reload the value and try again.");

    drainGtkEvents();
    fixture.window().unset_child();
  }

  TEST_CASE("TrackFieldGrid - built-in parse and submission failures restore display and notify",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.detail_builtin_metadata_failure_test",
                           [&trackId](library::MusicLibrary& musicLibrary)
                           { trackId = library::test::addTrack(musicLibrary, {.title = "Before", .year = 2020}); }};
    auto& runtime = fixture.runtime();
    auto const navigation = ao::test::requireValue(runtime.workspace().navigateTo(rt::GlobalViewKind::AllTracks));
    REQUIRE(runtime.views().setSelection(navigation, {trackId}));
    drainGtkEvents();
    auto const componentPtr =
      fixture.create(LayoutNode{.type = "track.detailScope", .children = {LayoutNode{.type = "track.fieldGrid"}}});
    REQUIRE(componentPtr != nullptr);
    auto& root = componentPtr->widget();
    fixture.window().set_child(root);
    auto const editors = collectAll<track_field_grid::DetailFieldEditor>(root);
    auto const titleEditorIter =
      std::ranges::find_if(editors, [](auto const* editor) { return editor->text().raw() == "Before"; });
    auto const yearEditorIter =
      std::ranges::find_if(editors, [](auto const* editor) { return editor->text().raw() == "2020"; });
    REQUIRE(titleEditorIter != editors.end());
    REQUIRE(yearEditorIter != editors.end());
    auto* const titleEditor = *titleEditorIter;
    auto* const yearEditor = *yearEditorIter;

    titleEditor->startEditing();
    titleEditor->entry().set_text("After");
    REQUIRE(runtime.library().writer().createList(
      rt::LibraryWriter::ListDraft{.kind = rt::LibraryWriter::ListKind::Manual, .name = "Unrelated"}));
    titleEditor->stopEditing(true);

    CHECK(titleEditor->text().raw() == "Before");
    CHECK(trackSpecFor(runtime.musicLibrary(), trackId).title == "Before");
    auto feed = runtime.notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);
    auto const notificationCount = feed.entries.size();

    yearEditor->startEditing();
    yearEditor->entry().set_text("not-a-year");
    yearEditor->stopEditing(true);

    CHECK(yearEditor->text().raw() == "2020");
    CHECK(trackSpecFor(runtime.musicLibrary(), trackId).year == 2020);
    feed = runtime.notifications().feed();
    REQUIRE(feed.entries.size() == notificationCount + 1);
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);

    drainGtkEvents();
    fixture.window().unset_child();
  }

  TEST_CASE("TrackDetailScope - clears pending detail undo when selection changes",
            "[gtk][unit][layout-component][semantic]")
  {
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_undo_scope_test",
                                        [&](library::MusicLibrary& musicLibrary)
                                        {
                                          firstTrackId = library::test::addTrack(
                                            musicLibrary, {.title = "First", .customMetadata = {{"Mood", "Bright"}}});
                                          secondTrackId = library::test::addTrack(musicLibrary, {.title = "Second"});
                                        }};
    auto& runtime = fixture.runtime();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigateTo(rt::kAllTracksListId));
    REQUIRE(runtime.views().setSelection(viewId, {firstTrackId}));
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

    REQUIRE(runtime.views().setSelection(viewId, {secondTrackId}));
    drainGtkEvents();

    CHECK_FALSE(undoBar->get_visible());
  }

  TEST_CASE("TrackDetailUndoBar - restores deleted custom metadata from button",
            "[gtk][unit][layout-component][semantic]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.detail_undo_button_test",
                           [&trackId](library::MusicLibrary& musicLibrary)
                           {
                             trackId = library::test::addTrack(
                               musicLibrary, {.title = "Undo Button Target", .customMetadata = {{"Mood", "Bright"}}});
                           }};
    auto& runtime = fixture.runtime();
    auto const& musicLibrary = runtime.musicLibrary();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigateTo(rt::kAllTracksListId));
    REQUIRE(runtime.views().setSelection(viewId, {trackId}));
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

  TEST_CASE("TrackFieldGrid - add custom metadata writes metadata and clears stale delete undo",
            "[gtk][unit][layout-component][semantic]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.detail_add_custom_test",
                           [&trackId](library::MusicLibrary& musicLibrary)
                           {
                             trackId = library::test::addTrack(
                               musicLibrary, {.title = "Add Target", .customMetadata = {{"Mood", "Bright"}}});
                           }};
    auto& runtime = fixture.runtime();
    auto const& musicLibrary = runtime.musicLibrary();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigateTo(rt::kAllTracksListId));
    REQUIRE(runtime.views().setSelection(viewId, {trackId}));
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

  TEST_CASE("track.quickFilter - wires create smart list action", "[gtk][unit][layout-component][semantic]")
  {
    [[maybe_unused]] auto const appPtr = ao::gtk::test::ensureGtkApplication();
    auto fixture = ao::gtk::test::GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library()};
    auto window = Gtk::Window{};
    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{window, runtime, std::move(tagEditCallbacks), themeCoordinator};
    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{window, runtime, std::move(navCallbacks), themeCoordinator};
    auto layoutStore = uimodel::TrackColumnLayoutStore{};
    auto pageHost = TrackPageHost{stack, runtime, tagEditController, listNavigation, layoutStore};

    REQUIRE(runtime.workspace().navigateTo(rt::kAllTracksListId));
    drainGtkEvents();

    pageHost.rebuild(cache);
    drainGtkEvents();

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto actionRegistry = ActionRegistry{};
    auto runtimeState = LayoutRuntimeState{};
    auto dependencies = GtkUiDependencies{};
    auto ctx = LayoutBuildContext{.registry = registry,
                                  .actionRegistry = actionRegistry,
                                  .runtime = runtime,
                                  .parentWindow = window,
                                  .runtimeState = runtimeState,
                                  .buildState = LayoutBuildStateView{runtimeState},
                                  .dependencies = dependencies};
    dependencies.trackPageHost = &pageHost;
    auto pendingDebounce = sigc::slot<bool()>{};
    ctx.timeoutScheduler = [&](std::chrono::milliseconds interval, sigc::slot<bool()> callback)
    {
      CHECK(interval == std::chrono::milliseconds{200});
      pendingDebounce = std::move(callback);
      return sigc::connection{};
    };
    auto capturedParentId = kInvalidListId;
    auto capturedExpression = std::string{};
    dependencies.createSmartListFromExpression = [&](ListId parentListId, std::string expression)
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
