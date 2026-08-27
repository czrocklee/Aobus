// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutCollaborators.h"
#include "app/ThemeCoordinator.h"
#include "app/linux-gtk/image/CoverArtView.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/image/ResourceImageLoader.h"
#include "app/linux-gtk/layout/component/semantic/SemanticComponentRegistrations.h"
#include "app/linux-gtk/layout/component/track/TrackComponentRegistrations.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/track/TrackRowCache.h"
#include "layout/component/track/TrackDetailUndo.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "list/ListNavigationController.h"
#include "portal/ImportExportActions.h"
#include "tag/TagEditController.h"
#include "tag/TagEditor.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "track/TrackPageHost.h"
#include "track/TrackQuickFilter.h"
#include <ao/CoreIds.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/property/TrackAuthoringSession.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

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
#include <pangomm/layout.h>
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
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk::layout::test
{
  using ao::gtk::test::pumpGtkEventsUntil;
  using ao::gtk::test::runGtkTask;

  using namespace uimodel;
  using ao::gtk::test::collectAll;
  using ao::gtk::test::directChildLabelTextsByClass;
  using ao::gtk::test::drainGtkEvents;
  using ao::gtk::test::emitClicked;
  using ao::gtk::test::findButtonByLabel;
  using ao::gtk::test::findLabelByText;
  using ao::gtk::test::findWidget;
  using ao::gtk::test::findWidgetByClass;
  using ao::gtk::test::measureWidget;

  namespace
  {
    constexpr std::size_t kOversizedMetadataLength = std::size_t{1024} * 1024;

    class RecordingImportExportActions final : public portal::ImportExportActions
    {
    public:
      void openLibrary() override { ++_openLibraryCount; }
      void scanLibrary() override {}
      void importLibrary() override {}
      void exportLibrary() override {}

      std::int32_t openLibraryCount() const noexcept { return _openLibraryCount; }

    private:
      std::int32_t _openLibraryCount = 0;
    };

    library::test::TrackSpec trackSpecFor(library::MusicLibrary const& musicLibrary, TrackId const trackId)
    {
      auto const transaction = musicLibrary.readTransaction();
      auto const optView =
        musicLibrary.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      return library::test::trackSpecFromView(musicLibrary, *optView);
    }

    bool hasNotification(rt::NotificationService& notifications,
                         rt::NotificationSeverity const severity,
                         std::string_view const message)
    {
      auto const feed = notifications.feed();
      return std::ranges::any_of(feed.entries,
                                 [severity, message](auto const& entry)
                                 {
                                   auto const* text = std::get_if<std::string>(&entry.message);
                                   return entry.severity == severity && text != nullptr && *text == message;
                                 });
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
      auto const rdpPtr =
        std::make_unique<TrackRowCache>(fixture.runtime().library(), ao::test::englishMessageCatalog());
      registerListTreeComponent(fixture.components(), rdpPtr.get(), nullptr);
      auto const node = LayoutNode{.type = "library.listTree"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().raw().contains("listNavigationController missing"));
    }

    SECTION("track.table shows error when trackPageGraph missing")
    {
      auto const node = LayoutNode{.type = "track.table"};
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

    SECTION("track.coverArt shows error when image loader is missing")
    {
      auto const compPtr = fixture.create(LayoutNode{.type = "track.coverArt"});

      REQUIRE(compPtr != nullptr);
      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().raw().contains("imageLoader missing"));
    }

    SECTION("library.openLibraryButton is disabled when its action service is missing")
    {
      auto const compPtr = fixture.create(LayoutNode{.type = "library.openLibraryButton"});

      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK_FALSE(button->get_sensitive());
    }
  }

  TEST_CASE("SemanticLayoutComponents - render configured GTK widgets", "[gtk][unit][layout-component][semantic]")
  {
    auto undoTrackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.layout_test",
                                        [&undoTrackId](library::MusicLibrary& musicLibrary)
                                        {
                                          undoTrackId = library::test::addTrackWithUniqueFixtureUri(
                                            musicLibrary, {.title = "Undo notification target"});
                                        }};
    auto& ctx = fixture.context();

    int const cacheSize = 10;
    auto imageCachePtr = std::make_unique<ImageCache>(cacheSize);
    auto byteLoader = rt::ResourceByteLoader{fixture.runtime()};
    auto imageLoaderPtr = std::make_unique<ResourceImageLoader>(byteLoader, *imageCachePtr, fixture.runtime().async());
    auto menuModelPtr = Gio::Menu::create();
    menuModelPtr->append_submenu("Test Menu", Gio::Menu::create());
    registerTrackCoverArtComponent(fixture.components(), imageLoaderPtr.get(), ao::test::englishMessageCatalog());
    registerMenuBarComponent(fixture.components(), menuModelPtr);
    registerMenuButtonComponent(fixture.components(), menuModelPtr, ao::test::englishMessageCatalog());

    {
      auto const node = LayoutNode{.type = "status.message"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Aobus Ready");
    }

    SECTION("library.openLibraryButton creates Gtk::Button")
    {
      auto actions = RecordingImportExportActions{};
      registerOpenLibraryButtonComponent(fixture.components(), &actions, ao::test::englishMessageCatalog());
      auto const node = LayoutNode{.type = "library.openLibraryButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "folder-open-symbolic");
      CHECK(btn->get_tooltip_text() == "Open Library...");
      CHECK(ao::gtk::test::hasAccessibleLabel(*btn, "Open Library..."));
      CHECK(btn->get_sensitive());

      emitClicked(*btn);
      CHECK(actions.openLibraryCount() == 1);
    }

    SECTION("app.menuBar creates Gtk::PopoverMenuBar")
    {
      auto const node = LayoutNode{.type = "app.menuBar"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const menuBar = dynamic_cast<Gtk::PopoverMenuBar*>(&compPtr->widget());
      CHECK(menuBar != nullptr);
    }

    SECTION("menuButton creates Gtk::MenuButton and sets menu model")
    {
      auto const node = LayoutNode{.type = "menuButton", .props = {{"icon", LayoutValue{"test-icon"}}}};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const menuButton = dynamic_cast<Gtk::MenuButton*>(&compPtr->widget());
      REQUIRE(menuButton != nullptr);
      auto windowFixture = ao::gtk::test::GtkWindowFixture{};
      windowFixture.mount(compPtr->widget());
      windowFixture.present();
      CHECK(menuButton->get_icon_name() == "test-icon");
      CHECK(menuButton->get_menu_model() == menuModelPtr);
      CHECK(ao::gtk::test::hasAccessibleLabel(*menuButton, "Application menu"));
    }

    SECTION("app.menuBar tolerates absent menu model")
    {
      registerMenuBarComponent(fixture.components(), nullptr);
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
      CHECK(imageWidget->get_width() == 180);
      CHECK(imageWidget->get_height() == 180);

      fixture.window().unset_child();
    }

    SECTION("track.coverArt displays a no-cover placeholder for missing art")
    {
      auto& scope = fixture.attachTrackDetailScope();
      auto node = LayoutNode{.type = "track.coverArt"};
      node.props["placeholderStyle"] = LayoutValue{"soul"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(compPtr->widget().get_first_child());
      REQUIRE(coverArt != nullptr);
      CHECK_FALSE(coverArt->get_visible());

      auto selected = rt::TrackDetailSnapshot{};
      selected.selectionKind = rt::SelectionKind::Single;
      selected.trackIds = {undoTrackId};
      scope.setSnapshot(std::move(selected));
      drainGtkEvents();

      CHECK(coverArt->get_visible());
      CHECK(coverArt->showingPlaceholder());
      CHECK(coverArt->placeholderPresentation().style == CoverArtPlaceholderStyle::Soul);
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
      REQUIRE(pumpGtkEventsUntil([&fixture] { return !fixture.runtime().notifications().feed().entries.empty(); }));

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

  TEST_CASE("TrackTagEditorComponent - forwards runtime text order to tag suggestions",
            "[gtk][unit][layout-component][collation]")
  {
    auto policyRes = i18n::createIcuTextOrderingPolicy("de-DE");
    REQUIRE(policyRes);
    auto policyPtr = std::move(*policyRes);
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.tag_editor_collation_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      {
        trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Tag Target"});
        library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Zulu Tag Source", .tags = {"z"}});
        library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Umlaut Tag Source", .tags = {"ä"}});
      },
      "de",
      policyPtr.get()};
    auto snapshot = rt::TrackDetailSnapshot{};
    snapshot.selectionKind = rt::SelectionKind::Single;
    snapshot.trackIds = {trackId};
    fixture.attachTrackDetailScope(std::move(snapshot));

    auto const componentPtr = fixture.create(LayoutNode{.type = "track.tagEditor"});
    REQUIRE(componentPtr != nullptr);
    auto* const editor = dynamic_cast<TagEditor*>(&componentPtr->widget());
    REQUIRE(editor != nullptr);
    CHECK(directChildLabelTextsByClass(*editor, "ao-tag-chip-suggested") == std::vector<std::string>{"ä", "z"});
  }

  TEST_CASE("TrackTagEditorComponent - stale completion preserves a replacement selection session",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.tag_editor_stale_fallback_test",
      [&](library::MusicLibrary& musicLibrary)
      {
        firstTrackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "First Target"});
        secondTrackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Second Target"});
      }};
    auto& runtime = fixture.runtime();
    auto& scope = fixture.attachTrackDetailScope();
    auto snapshot = rt::TrackDetailSnapshot{};
    snapshot.selectionKind = rt::SelectionKind::Single;
    snapshot.trackIds = {firstTrackId};
    scope.setSnapshot(std::move(snapshot));

    auto const componentPtr = fixture.create(LayoutNode{.type = "track.tagEditor"});
    REQUIRE(componentPtr != nullptr);
    auto* const editor = dynamic_cast<TagEditor*>(&componentPtr->widget());
    REQUIRE(editor != nullptr);

    auto const firstAddition = std::array{std::string{"First"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{firstAddition}, std::span<std::string const>{});

    auto const expectedTags = std::vector<std::string>{"First"};
    REQUIRE(pumpGtkEventsUntil([&runtime, firstTrackId, &expectedTags]
                               { return trackSpecFor(runtime.musicLibrary(), firstTrackId).tags == expectedTags; }));
    REQUIRE(pumpGtkEventsUntil([&runtime] { return !runtime.notifications().feed().entries.empty(); }));
    CHECK(trackSpecFor(runtime.musicLibrary(), firstTrackId).tags == expectedTags);
    auto feed = runtime.notifications().feed();
    REQUIRE_FALSE(feed.entries.empty());
    CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
    CHECK(std::get<std::string>(feed.entries.back().message) == "Tags added 1 for 1 track");

    REQUIRE(runGtkTask(runtime, runtime.library().writer().createList(rt::ListDraft{.name = "Unrelated"})));
    auto const secondAddition = std::array{std::string{"Second"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{secondAddition}, std::span<std::string const>{});

    auto replacementSnapshot = rt::TrackDetailSnapshot{};
    replacementSnapshot.selectionKind = rt::SelectionKind::Single;
    replacementSnapshot.trackIds = {secondTrackId};
    scope.setSnapshot(std::move(replacementSnapshot));
    auto const replacementAddition = std::array{std::string{"Replacement"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{replacementAddition}, std::span<std::string const>{});

    REQUIRE(pumpGtkEventsUntil(
      [&runtime, secondTrackId]
      {
        return trackSpecFor(runtime.musicLibrary(), secondTrackId).tags == std::vector<std::string>{"Replacement"} &&
               hasNotification(runtime.notifications(),
                               rt::NotificationSeverity::Error,
                               "Library changed while the tag editor was open. Reload and try again.");
      }));
    CHECK(trackSpecFor(runtime.musicLibrary(), firstTrackId).tags == expectedTags);
    drainGtkEvents();

    REQUIRE(runGtkTask(runtime, runtime.library().writer().createList(rt::ListDraft{.name = "Invalidate Second"})));
    auto const notificationCount = runtime.notifications().feed().entries.size();
    auto const retryAddition = std::array{std::string{"Retry"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{retryAddition}, std::span<std::string const>{});
    REQUIRE(pumpGtkEventsUntil([&runtime, notificationCount]
                               { return runtime.notifications().feed().entries.size() > notificationCount; }));

    CHECK(trackSpecFor(runtime.musicLibrary(), secondTrackId).tags == std::vector<std::string>{"Replacement"});
    CHECK(runtime.notifications().feed().entries.back().severity == rt::NotificationSeverity::Error);
  }

  TEST_CASE("TrackTagEditorComponent - fallback reports a concurrent retry as busy",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.tag_editor_busy_fallback_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Tag Target"}); }};
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
    auto const secondAddition = std::array{std::string{"Second"}};
    editor->signalTagsChanged().emit(std::span<std::string const>{firstAddition}, std::span<std::string const>{});
    editor->signalTagsChanged().emit(std::span<std::string const>{secondAddition}, std::span<std::string const>{});

    REQUIRE(pumpGtkEventsUntil(
      [&runtime]
      {
        return hasNotification(
          runtime.notifications(), rt::NotificationSeverity::Warning, "Library is busy. Try again.");
      }));
    REQUIRE(pumpGtkEventsUntil([&runtime, trackId]
                               { return trackSpecFor(runtime.musicLibrary(), trackId).tags.size() == 1; }));
  }

  TEST_CASE("TrackDetailUndoController - restores deleted custom metadata", "[gtk][unit][layout-component][semantic]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_undo_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Undo Target"}); }};
    auto const& musicLibrary = fixture.runtime().musicLibrary();
    auto undoController = TrackDetailUndoController{};
    auto sessionPtr =
      ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{trackId}));

    undoController.presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(sessionPtr));
    REQUIRE(runGtkTask(fixture.runtime(), undoController.undo()));

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
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_undo_timeout_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Undo timeout target"}); }};
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
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_stale_undo_test",
                                        [&trackId](library::MusicLibrary& musicLibrary)
                                        {
                                          trackId = library::test::addTrackWithUniqueFixtureUri(
                                            musicLibrary, {.customMetadata = {{"Mood", "Bright"}}});
                                        }};
    auto& runtime = fixture.runtime();
    auto sessionPtr = ao::test::requireValue(TrackAuthoringSession::begin(runtime.library(), std::array{trackId}));
    auto deletePatch = rt::MetadataPatch{};
    deletePatch.customUpdates["Mood"] = std::nullopt;
    auto deleteRes = runGtkTask(runtime, sessionPtr->submitMetadata(deletePatch));
    REQUIRE(deleteRes);
    REQUIRE(deleteRes->status == rt::AuthoringStatus::Applied);
    auto controller = TrackDetailUndoController{};
    controller.presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(sessionPtr));

    REQUIRE(runGtkTask(runtime, runtime.library().writer().createList(rt::ListDraft{.name = "Unrelated"})));
    REQUIRE(controller.pendingCustomMetadataUndo());
    CHECK_FALSE(controller.pendingCustomMetadataUndo()->sessionPtr->isCurrent());

    auto const undoRes = runGtkTask(runtime, controller.undo());

    REQUIRE_FALSE(undoRes);
    CHECK(undoRes.error().message == "Library changed before metadata undo could be applied");
    CHECK_FALSE(controller.pendingCustomMetadataUndo());
    CHECK(trackSpecFor(runtime.musicLibrary(), trackId).customMetadata.empty());
  }

  TEST_CASE("TrackDetailUndoController - rejected undo clears the terminal action",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_rejected_undo_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Rejected undo target"}); }};
    auto sessionPtr =
      ao::test::requireValue(TrackAuthoringSession::begin(fixture.runtime().library(), std::array{trackId}));
    auto controller = TrackDetailUndoController{};
    std::size_t changedCount = 0;
    auto changedConnection = controller.signalChanged().connect([&changedCount] { ++changedCount; });

    controller.presentCustomMetadataDeletedUndo(
      "Mood", std::string(kOversizedMetadataLength, 'x'), std::move(sessionPtr));
    REQUIRE(controller.pendingCustomMetadataUndo());

    auto const undoRes = runGtkTask(fixture.runtime(), controller.undo());

    REQUIRE_FALSE(undoRes);
    CHECK_FALSE(controller.pendingCustomMetadataUndo());
    CHECK(changedCount == 2);
    CHECK(trackSpecFor(fixture.runtime().musicLibrary(), trackId).customMetadata.empty());
  }

  TEST_CASE("TrackDetailUndoController - publication may destroy the controller before undo settles",
            "[gtk][regression][track-detail-undo][concurrency]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_undo_teardown_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Undo teardown target"}); }};
    auto& runtime = fixture.runtime();
    auto controllerPtr = std::make_unique<TrackDetailUndoController>();
    auto sessionPtr = ao::test::requireValue(TrackAuthoringSession::begin(runtime.library(), std::array{trackId}));
    controllerPtr->presentCustomMetadataDeletedUndo("Mood", "Bright", std::move(sessionPtr));
    bool controllerDestroyed = false;
    auto changedSubscription = runtime.library().changes().onChanged(
      [&](rt::LibraryChangeSet const&)
      {
        controllerPtr.reset();
        controllerDestroyed = true;
      });

    auto undoTask = controllerPtr->undo();
    auto const undoRes = runGtkTask(runtime, std::move(undoTask));

    REQUIRE(undoRes);
    CHECK(controllerDestroyed);
    CHECK(controllerPtr == nullptr);
    auto const spec = trackSpecFor(runtime.musicLibrary(), trackId);
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata.front() == std::pair{std::string{"Mood"}, std::string{"Bright"}});
  }

  TEST_CASE("TrackFieldGrid - a stale authoring session cancels the active editor",
            "[gtk][unit][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_stale_editor_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Before"}); }};
    auto& runtime = fixture.runtime();
    auto const navigation =
      ao::test::requireValue(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));
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
    REQUIRE(runGtkTask(runtime, runtime.library().writer().createList(rt::ListDraft{.name = "Unrelated"})));
    drainGtkEvents();

    CHECK_FALSE(titleEditor->isEditing());
    CHECK(titleEditor->text().raw() == "Before");
    fixture.window().unset_child();
  }

  TEST_CASE("TrackFieldGrid - an intervening revision cancels custom metadata editing without changing storage",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.detail_stale_custom_metadata_test",
                                        [&trackId](library::MusicLibrary& musicLibrary)
                                        {
                                          trackId = library::test::addTrackWithUniqueFixtureUri(
                                            musicLibrary, {.customMetadata = {{"Mood", "Bright"}}});
                                        }};
    auto& runtime = fixture.runtime();
    auto const navigation =
      ao::test::requireValue(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));
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
    REQUIRE(runGtkTask(runtime, runtime.library().writer().createList(rt::ListDraft{.name = "Unrelated"})));

    REQUIRE(pumpGtkEventsUntil([moodEditor] { return !moodEditor->isEditing(); }));

    auto const spec = trackSpecFor(runtime.musicLibrary(), trackId);
    REQUIRE(spec.customMetadata.size() == 1);
    auto const expectedMetadata = std::pair{std::string{"Mood"}, std::string{"Bright"}};
    CHECK(spec.customMetadata.front() == expectedMetadata);
    auto const feed = runtime.notifications().feed();
    CHECK(feed.entries.empty());

    drainGtkEvents();
    fixture.window().unset_child();
  }

  TEST_CASE("TrackFieldGrid - built-in edit failures restore display and notify",
            "[gtk][regression][layout-component][library-authoring]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture = LayoutRuntimeFixture{
      "io.github.aobus.detail_builtin_metadata_failure_test",
      [&trackId](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Before", .year = 2020}); }};
    auto& runtime = fixture.runtime();
    auto const navigation =
      ao::test::requireValue(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));
    REQUIRE(runtime.views().setSelection(navigation, {trackId}));
    drainGtkEvents();
    auto const componentPtr =
      fixture.create(LayoutNode{.type = "track.detailScope", .children = {LayoutNode{.type = "track.fieldGrid"}}});
    REQUIRE(componentPtr != nullptr);
    auto& root = componentPtr->widget();
    fixture.window().set_child(root);
    auto const editors = collectAll<track_field_grid::DetailFieldEditor>(root);

    SECTION("parse failures")
    {
      auto const editorIter =
        std::ranges::find_if(editors, [](auto const* editor) { return editor->text().raw() == "2020"; });
      REQUIRE(editorIter != editors.end());
      auto* const editor = *editorIter;

      editor->startEditing();
      editor->entry().set_text("not-a-year");
      editor->stopEditing(true);

      CHECK(editor->text().raw() == "2020");
      CHECK(trackSpecFor(runtime.musicLibrary(), trackId).year == 2020);
      auto const feed = runtime.notifications().feed();
      REQUIRE_FALSE(feed.entries.empty());
      CHECK(feed.entries.back().severity == rt::NotificationSeverity::Error);
    }

    SECTION("concurrent submissions")
    {
      auto const editorIter =
        std::ranges::find_if(editors, [](auto const* editor) { return editor->text().raw() == "Before"; });
      REQUIRE(editorIter != editors.end());
      auto* const editor = *editorIter;

      editor->startEditing();
      editor->entry().set_text("First");
      editor->stopEditing(true);
      editor->startEditing();
      editor->entry().set_text("Second");
      editor->stopEditing(true);

      REQUIRE(pumpGtkEventsUntil(
        [&runtime]
        {
          return hasNotification(
            runtime.notifications(), rt::NotificationSeverity::Warning, "Library is busy. Try again.");
        }));
      REQUIRE(pumpGtkEventsUntil([&runtime, trackId]
                                 { return trackSpecFor(runtime.musicLibrary(), trackId).title != "Before"; }));
    }

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
                                          firstTrackId = library::test::addTrackWithUniqueFixtureUri(
                                            musicLibrary, {.title = "First", .customMetadata = {{"Mood", "Bright"}}});
                                          secondTrackId = library::test::addTrackWithUniqueFixtureUri(
                                            musicLibrary, {.title = "Second"});
                                        }};
    auto& runtime = fixture.runtime();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
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
    REQUIRE(pumpGtkEventsUntil([undoBar] { return undoBar->get_visible(); }));

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
                             trackId = library::test::addTrackWithUniqueFixtureUri(
                               musicLibrary, {.title = "Undo Button Target", .customMetadata = {{"Mood", "Bright"}}});
                           }};
    auto& runtime = fixture.runtime();
    auto const& musicLibrary = runtime.musicLibrary();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
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
    REQUIRE(pumpGtkEventsUntil(
      [&musicLibrary, trackId, undoBar]
      { return trackSpecFor(musicLibrary, trackId).customMetadata.empty() && undoBar->get_visible(); }));

    CHECK(trackSpecFor(musicLibrary, trackId).customMetadata.empty());
    CHECK(undoBar->get_visible());
    auto* const undoLabel = findLabelByText(root, "Custom metadata 'Mood' removed");
    REQUIRE(undoLabel != nullptr);
    CHECK(undoLabel->get_ellipsize() == Pango::EllipsizeMode::END);
    CHECK(undoLabel->get_tooltip_text() == "Custom metadata 'Mood' removed");

    auto* const undoButton = findWidgetByClass<Gtk::Button>(root, "ao-undo-button");
    REQUIRE(undoButton != nullptr);
    emitClicked(*undoButton);
    emitClicked(*undoButton);
    REQUIRE(pumpGtkEventsUntil(
      [&musicLibrary, trackId, undoBar]
      { return !trackSpecFor(musicLibrary, trackId).customMetadata.empty() && !undoBar->get_visible(); }));

    auto const spec = trackSpecFor(musicLibrary, trackId);
    REQUIRE(spec.customMetadata.size() == 1);
    CHECK(spec.customMetadata[0].first == "Mood");
    CHECK(spec.customMetadata[0].second == "Bright");
    CHECK_FALSE(undoBar->get_visible());
    CHECK(hasNotification(runtime.notifications(), rt::NotificationSeverity::Warning, "Library is busy. Try again."));
  }

  TEST_CASE("TrackFieldGrid - add custom metadata writes metadata and clears stale delete undo",
            "[gtk][unit][layout-component][semantic]")
  {
    auto trackId = kInvalidTrackId;
    auto fixture =
      LayoutRuntimeFixture{"io.github.aobus.detail_add_custom_test",
                           [&trackId](library::MusicLibrary& musicLibrary)
                           {
                             trackId = library::test::addTrackWithUniqueFixtureUri(
                               musicLibrary, {.title = "Add Target", .customMetadata = {{"Mood", "Bright"}}});
                           }};
    auto& runtime = fixture.runtime();
    auto const& musicLibrary = runtime.musicLibrary();

    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
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

    auto* const undoBar = findWidgetByClass<Gtk::Widget>(root, "ao-undo-bar");
    REQUIRE(undoBar != nullptr);
    REQUIRE(pumpGtkEventsUntil(
      [&musicLibrary, trackId, undoBar]
      { return trackSpecFor(musicLibrary, trackId).customMetadata.empty() && undoBar->get_visible(); }));
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
    REQUIRE(pumpGtkEventsUntil(
      [&musicLibrary, trackId, popover, undoBar]
      {
        auto const current = trackSpecFor(musicLibrary, trackId);
        return current.customMetadata.size() == 1 && current.customMetadata.front().second == "Dark" &&
               !popover->get_visible() && !undoBar->get_visible();
      }));

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
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};
    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{
      window, runtime, ao::test::englishMessageCatalog(), std::move(tagEditCallbacks), themeCoordinator};
    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{
      window, runtime, ao::test::englishMessageCatalog(), std::move(navCallbacks), themeCoordinator};
    auto layoutStore = uimodel::TrackColumnLayoutStore{};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto pageHost = TrackPageHost{
      stack, runtime, tagEditController, listNavigation, layoutStore, ao::test::englishMessageCatalog(), byteLoader};

    REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
    drainGtkEvents();

    pageHost.rebuild(cache);
    drainGtkEvents();

    auto capturedParentId = kInvalidListId;
    auto capturedExpression = std::string{};
    auto const createSmartListFromExpression = [&](ListId parentListId, std::string expression)
    {
      capturedParentId = parentListId;
      capturedExpression = std::move(expression);
    };

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry,
                                              runtime,
                                              ShellLayoutCollaborators{
                                                .textCatalog = ao::test::englishMessageCatalog(),
                                                .trackPageHost = &pageHost,
                                                .createSmartListFromExpression = createSmartListFromExpression,
                                              });

    auto actionRegistry = ActionRegistry{};
    auto runtimeState = uimodel::LayoutRuntimeState{};
    auto ctx = LayoutBuildContext{.registry = registry,
                                  .actionRegistry = actionRegistry,
                                  .parentWindow = window,
                                  .runtimeState = runtimeState,
                                  .buildState = uimodel::LayoutBuildStateView{runtimeState}};
    auto pendingDebounce = sigc::slot<bool()>{};
    ctx.timeoutScheduler = [&](std::chrono::milliseconds interval, sigc::slot<bool()> callback)
    {
      CHECK(interval == std::chrono::milliseconds{200});
      pendingDebounce = std::move(callback);
      return sigc::connection{};
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
