// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "app/ThemeCoordinator.h"
#include "list/ListNavigationController.h"
#include "runtime/PlaybackSessionState.h"
#include "runtime/PlaybackSessionYamlSchema.h"
#include "tag/TagEditController.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkNativeInputTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "track/TrackListItem.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/LibraryWrite.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/columnviewsorter.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/multiselection.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    Gtk::ColumnView& currentColumnView(TrackPageHost& host)
    {
      auto const* const entry = host.currentVisible();
      REQUIRE(entry != nullptr);
      REQUIRE(entry->pagePtr != nullptr);
      auto* const view = findWidget<Gtk::ColumnView>(*entry->pagePtr);
      REQUIRE(view != nullptr);
      return *view;
    }

    Glib::RefPtr<Gtk::ColumnViewSorter> currentHeaderSorter(TrackPageHost& host)
    {
      auto const sorterPtr = std::dynamic_pointer_cast<Gtk::ColumnViewSorter>(currentColumnView(host).get_sorter());
      REQUIRE(sorterPtr != nullptr);
      return sorterPtr;
    }

    void checkSortArrow(TrackPageHost& host, rt::TrackField const field, Gtk::SortType const direction)
    {
      auto const sorterPtr = currentHeaderSorter(host);
      CHECK(sorterPtr->get_n_sort_columns() == 1);
      auto const columnPtr = sorterPtr->get_primary_sort_column();
      REQUIRE(columnPtr != nullptr);
      CHECK(columnPtr->get_id().raw() == rt::trackFieldId(field));
      CHECK(sorterPtr->get_primary_sort_order() == direction);
    }

    void clickHeader(Gtk::Stack& stack, TrackPageHost& host, GtkNativePointerFixture& pointer, std::string const& label)
    {
      double x = 0;
      double y = 0;
      REQUIRE(tryPumpGtkEventsUntil(
        [&]
        {
          auto* const entry = host.currentVisible();
          auto* const header = entry ? findLabelByText(*entry->pagePtr, label) : nullptr;
          return header && header->get_mapped() && header->get_width() > 0 && header->get_height() > 0 &&
                 header->translate_coordinates(stack, header->get_width() / 2.0, header->get_height() / 2.0, x, y);
        }));
      pointer.movePointerTo(x, y);
      pointer.press();
      pointer.release();
      drainGtkEvents();
    }

    Glib::RefPtr<Gtk::ColumnViewColumn> columnForField(Gtk::ColumnView& view, rt::TrackField const field)
    {
      auto const columnsPtr = view.get_columns();
      REQUIRE(columnsPtr != nullptr);

      for (std::uint32_t index = 0; index < columnsPtr->get_n_items(); ++index)
      {
        auto const columnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(index));

        if (columnPtr && columnPtr->get_id().raw() == rt::trackFieldId(field))
        {
          return columnPtr;
        }
      }

      return {};
    }

    std::string firstVisibleTrackTitle(Gtk::ColumnView& view)
    {
      auto* const heading = findLabelByText(view, "Title");
      double headerX = 0;
      double headerBottom = 0;

      if (heading == nullptr || !heading->get_mapped() ||
          !heading->translate_coordinates(view, 0, heading->get_height(), headerX, headerBottom))
      {
        return {};
      }

      // GTK also keeps prefetched row widgets. Inspect the title nearest the
      // header within the viewport, not the first label in widget-tree order.
      double firstY = view.get_height();
      auto title = std::string{};

      for (auto* const label : collectAll<Gtk::Label>(view))
      {
        if (!label->get_mapped() || !hasCssClass(*label, "ao-track-title-cell"))
        {
          continue;
        }

        double x = 0;

        if (double y = 0;
            label->get_width() > 0 && label->get_height() > 0 &&
            label->translate_coordinates(view, label->get_width() / 2.0, label->get_height() / 2.0, x, y) && x >= 0 &&
            x < view.get_width() && y >= headerBottom && y < firstY)
        {
          firstY = y;
          title = label->get_text().raw();
        }
      }

      return title;
    }

    TrackId restoreRevealTarget(rt::AppRuntimeOwner& owner)
    {
      auto& runtime = owner.runtime();
      auto const trackId = addRuntimeTrack(owner, {.title = "Restored reveal target"});
      runtime.sources().reloadAllTracks();
      auto const snapshot = runtime.library().snapshot();
      auto const optUri = snapshot.trackUri(trackId);
      REQUIRE(optUri);
      auto const session = rt::PlaybackSessionState{.libraryId = snapshot.libraryId(),
                                                    .sourceListId = rt::kAllTracksListId,
                                                    .currentTrackId = trackId,
                                                    .currentTrackUri = *optUri};
      REQUIRE(
        runtime.workspaceConfigStore().save(rt::kPlaybackSessionConfigGroup, session, rt::PlaybackSessionYamlSchema{}));
      REQUIRE(ao::test::requireValue(runtime.restorePlaybackSession()).restored);
      return trackId;
    }
  } // namespace

  TEST_CASE("TrackPageHost - binds runtime pages to the GTK stack")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};

    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{window,
                                               runtime.async(),
                                               fixture.editing(),
                                               runtime.completion(),
                                               runtime.notifications(),
                                               runtime.textOrderingPolicy(),
                                               ao::test::englishMessageCatalog(),
                                               std::move(tagEditCallbacks),
                                               themeCoordinator};

    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{
      window, runtime, fixture.editing(), ao::test::englishMessageCatalog(), std::move(navCallbacks), themeCoordinator};

    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto host = TrackPageHost{stack,
                              runtime.async(),
                              fixture.editing(),
                              runtime.playback(),
                              runtime.views(),
                              runtime.workspace(),
                              tagEditController,
                              listNavigation,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.resourceBytes()};

    SECTION("initial state")
    {
      CHECK(host.currentVisible() == nullptr);
    }

    SECTION("rebuild creating pages")
    {
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      drainGtkEvents();

      host.rebuild(cache);
      drainGtkEvents();

      auto const* const current = host.currentVisible();
      REQUIRE(current != nullptr);
      REQUIRE(current->pagePtr != nullptr);
      CHECK(current->viewId == viewId);
      CHECK(current->pagePtr->listId() == rt::kAllTracksListId);
      CHECK(host.activeListId() == rt::kAllTracksListId);
    }

    SECTION("only explicit header sorts show an arrow and reset direction when changing columns")
    {
      requireOwnedGtkDisplay();
      auto const firstTrack = addRuntimeTrack(fixture.owner(), {.title = "Alpha", .year = 1990});
      auto const lastTrack = addRuntimeTrack(fixture.owner(), {.title = "Zulu", .year = 2000});
      runtime.sources().reloadAllTracks();
      auto const otherListId = ao::test::requireValue(
        runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Other page"})));
      auto const otherViewId = ao::test::requireValue(runtime.workspace().navigate({.target = otherListId}));
      auto const& preset = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Songs);
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation = rt::NavigationPresentation{.spec = preset.spec},
      }));
      auto const original = runtime.views().trackListState(viewId).presentation;
      REQUIRE(original.isPreset(rt::BuiltinPreset::Songs));
      auto const otherPresentation = runtime.views().trackListState(otherViewId).presentation;
      auto const workspace = runtime.workspace().snapshot();

      host.rebuild(cache);
      window.set_child(stack);
      window.set_default_size(900, 400);
      window.present();
      drainGtkEvents();
      REQUIRE(runtime.views().trackListState(viewId).presentation == original);

      // A passive layout rebuild is not a manual sort. Only the subsequent
      // header activation changes it.
      auto const hideArtist =
        uimodel::TrackColumnState{.field = rt::TrackField::Artist, .visibility = uimodel::TrackColumnVisibility::Hide};
      columnLayouts.restore({{rt::kAllTracksListId, {hideArtist}}}, {});
      host.rebuild(cache);
      drainGtkEvents();
      REQUIRE(runtime.views().trackListState(viewId).presentation == original);
      auto pointer = GtkNativePointerFixture{stack};

      CHECK(currentHeaderSorter(host)->get_primary_sort_column() == nullptr);
      clickHeader(stack, host, pointer, "Title");
      auto expected = original;
      expected.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = true};
      CHECK(runtime.views().trackListState(viewId).presentation == expected);
      checkSortArrow(host, rt::TrackField::Title, Gtk::SortType::ASCENDING);
      auto const* const entry = host.find(viewId);
      REQUIRE(entry != nullptr);
      REQUIRE(entry->pagePtr->projection() != nullptr);
      CHECK(entry->pagePtr->projection()->trackIdAt(0) == firstTrack);
      auto& view = currentColumnView(host);
      auto const columnsPtr = view.get_columns();
      auto const selectionPtr = std::dynamic_pointer_cast<Gtk::MultiSelection>(view.get_model());
      REQUIRE(selectionPtr != nullptr);
      REQUIRE(selectionPtr->select_item(0, true));
      REQUIRE(runtime.views().trackListState(viewId).selection == std::vector{firstTrack});

      clickHeader(stack, host, pointer, "Title");
      expected.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = false};
      CHECK(runtime.views().trackListState(viewId).presentation == expected);
      checkSortArrow(host, rt::TrackField::Title, Gtk::SortType::DESCENDING);
      CHECK(currentColumnView(host).get_columns() == columnsPtr);
      REQUIRE(currentColumnView(host).get_model() == selectionPtr);
      REQUIRE(selectionPtr->get_n_items() == 2);
      auto const frontPtr = std::dynamic_pointer_cast<TrackListItem>(selectionPtr->get_object(0));
      auto const backPtr = std::dynamic_pointer_cast<TrackListItem>(selectionPtr->get_object(1));
      REQUIRE(frontPtr != nullptr);
      REQUIRE(backPtr != nullptr);
      CHECK(frontPtr->trackId() == lastTrack);
      CHECK(backPtr->trackId() == firstTrack);
      CHECK_FALSE(selectionPtr->is_selected(0));
      CHECK(selectionPtr->is_selected(1));
      CHECK(runtime.views().trackListState(viewId).selection == std::vector{firstTrack});

      clickHeader(stack, host, pointer, "Year");
      expected.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Year, .ascending = true};
      CHECK(runtime.views().trackListState(viewId).presentation == expected);
      CHECK(entry->pagePtr->projection()->trackIdAt(0) == firstTrack);
      checkSortArrow(host, rt::TrackField::Year, Gtk::SortType::ASCENDING);
      CHECK(runtime.views().trackListState(otherViewId).presentation == otherPresentation);
      CHECK(runtime.workspace().snapshot() == workspace);
    }

    SECTION("Albums Track # sorts discs and tracks while retaining its exact header arrow")
    {
      requireOwnedGtkDisplay();
      auto const discOne =
        addRuntimeTrack(fixture.owner(), {.title = "First Disc", .discNumber = 1, .discTotal = 2, .trackNumber = 2});
      auto const discTwo =
        addRuntimeTrack(fixture.owner(), {.title = "Second Disc", .discNumber = 2, .discTotal = 2, .trackNumber = 1});
      runtime.sources().reloadAllTracks();
      auto const& albums = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums).spec;
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation = rt::NavigationPresentation{.spec = albums},
      }));
      host.rebuild(cache);
      window.set_child(stack);
      window.set_default_size(900, 400);
      window.present();
      drainGtkEvents();
      auto pointer = GtkNativePointerFixture{stack};
      auto const* const entry = host.find(viewId);
      REQUIRE(entry != nullptr);
      REQUIRE(entry->pagePtr->projection() != nullptr);
      CHECK(currentHeaderSorter(host)->get_primary_sort_column() == nullptr);

      clickHeader(stack, host, pointer, "Track #");
      auto expected = albums;
      expected.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::DisplayTrackNumber, .ascending = true};
      CHECK(runtime.views().trackListState(viewId).presentation == expected);
      checkSortArrow(host, rt::TrackField::DisplayTrackNumber, Gtk::SortType::ASCENDING);
      CHECK(entry->pagePtr->projection()->trackIdAt(0) == discOne);

      clickHeader(stack, host, pointer, "Track #");
      expected.optColumnSort->ascending = false;
      CHECK(runtime.views().trackListState(viewId).presentation == expected);
      checkSortArrow(host, rt::TrackField::DisplayTrackNumber, Gtk::SortType::DESCENDING);
      CHECK(entry->pagePtr->projection()->trackIdAt(0) == discTwo);
    }

    SECTION("Movement and Movement No. each own their arrow despite sharing a sort key")
    {
      requireOwnedGtkDisplay();
      auto preset = rt::defaultTrackPresentationSpec();
      preset.visibleFields = {rt::TrackField::Movement, rt::TrackField::MovementNumber};
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation = rt::NavigationPresentation{.spec = preset},
      }));
      host.rebuild(cache);
      window.set_child(stack);
      window.set_default_size(900, 400);
      window.present();
      drainGtkEvents();
      auto pointer = GtkNativePointerFixture{stack};
      CHECK(currentHeaderSorter(host)->get_primary_sort_column() == nullptr);

      clickHeader(stack, host, pointer, "Movement");
      preset.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Movement, .ascending = true};
      CHECK(runtime.views().trackListState(viewId).presentation == preset);
      checkSortArrow(host, rt::TrackField::Movement, Gtk::SortType::ASCENDING);
      auto& view = currentColumnView(host);
      // GTK owns one column list for each ColumnView's lifetime.
      auto const columnsPtr = view.get_columns();
      auto const movementNumberPtr = columnForField(view, rt::TrackField::MovementNumber);
      REQUIRE(movementNumberPtr != nullptr);
      auto const width = movementNumberPtr->get_fixed_width();

      clickHeader(stack, host, pointer, "Movement No.");
      preset.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::MovementNumber, .ascending = true};
      CHECK(runtime.views().trackListState(viewId).presentation == preset);
      checkSortArrow(host, rt::TrackField::MovementNumber, Gtk::SortType::ASCENDING);
      auto& sortedView = currentColumnView(host);
      CHECK(sortedView.get_columns() == columnsPtr);
      auto const sortedMovementNumberPtr = columnForField(sortedView, rt::TrackField::MovementNumber);
      REQUIRE(sortedMovementNumberPtr != nullptr);
      CHECK(sortedMovementNumberPtr == movementNumberPtr);
      CHECK(sortedMovementNumberPtr->get_fixed_width() == width);

      clickHeader(stack, host, pointer, "Movement No.");
      preset.optColumnSort->ascending = false;
      CHECK(runtime.views().trackListState(viewId).presentation == preset);
      checkSortArrow(host, rt::TrackField::MovementNumber, Gtk::SortType::DESCENDING);

      clickHeader(stack, host, pointer, "Movement");
      preset.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Movement, .ascending = true};
      CHECK(runtime.views().trackListState(viewId).presentation == preset);
      checkSortArrow(host, rt::TrackField::Movement, Gtk::SortType::ASCENDING);
    }

    SECTION("programmatic overrides update or clear the exact header arrow in place")
    {
      auto preset = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Tagging).spec;
      preset.visibleFields.push_back(rt::TrackField::DisplayTrackNumber);
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation = rt::NavigationPresentation{.spec = preset},
      }));
      REQUIRE(runtime.workspace().snapshot().activeViewId == viewId);
      REQUIRE(runtime.views().setColumnSort(viewId, {.column = rt::TrackField::TrackNumber, .ascending = false}));
      host.rebuild(cache);
      drainGtkEvents();
      checkSortArrow(host, rt::TrackField::TrackNumber, Gtk::SortType::DESCENDING);
      auto const columnsPtr = currentColumnView(host).get_columns();

      auto const displaySort = rt::TrackColumnSort{.column = rt::TrackField::DisplayTrackNumber, .ascending = true};
      REQUIRE(runtime.views().setColumnSort(viewId, displaySort));
      drainGtkEvents();
      CHECK(currentColumnView(host).get_columns() == columnsPtr);
      checkSortArrow(host, rt::TrackField::DisplayTrackNumber, Gtk::SortType::ASCENDING);

      SECTION("reapplying the preset clears the previous arrow")
      {
        REQUIRE(runtime.workspace().setActivePresentation(preset, {.recordHistory = false}));
        drainGtkEvents();
        CHECK(currentColumnView(host).get_columns() == columnsPtr);
        CHECK(currentHeaderSorter(host)->get_primary_sort_column() == nullptr);
        CHECK(runtime.views().trackListState(viewId).presentation == preset);
      }

      SECTION("a hidden exact column does not put an arrow on its visible alias")
      {
        std::erase(preset.visibleFields, rt::TrackField::TrackNumber);
        preset.optColumnSort = displaySort;
        REQUIRE(runtime.workspace().setActivePresentation(preset, {.recordHistory = false}));
        drainGtkEvents();
        auto& view = currentColumnView(host);
        auto const newColumnsPtr = view.get_columns();
        CHECK(newColumnsPtr != columnsPtr);
        auto const trackNumberPtr = columnForField(view, rt::TrackField::TrackNumber);
        REQUIRE(trackNumberPtr != nullptr);
        REQUIRE_FALSE(trackNumberPtr->get_visible());
        checkSortArrow(host, rt::TrackField::DisplayTrackNumber, Gtk::SortType::ASCENDING);

        preset.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::TrackNumber, .ascending = true};
        REQUIRE(runtime.views().setColumnSort(viewId, *preset.optColumnSort));
        drainGtkEvents();
        CHECK(currentColumnView(host).get_columns() == newColumnsPtr);
        CHECK(currentHeaderSorter(host)->get_primary_sort_column() == nullptr);
        CHECK(runtime.views().trackListState(viewId).presentation == preset);
      }

      SECTION("a header change picks up a newer stored column layout")
      {
        auto const artistPtr = columnForField(currentColumnView(host), rt::TrackField::Artist);
        REQUIRE(artistPtr != nullptr);
        REQUIRE(artistPtr->get_visible());

        REQUIRE(runtime.views().setColumnSort(viewId, {.column = rt::TrackField::TrackNumber, .ascending = true}));
        auto const hideArtist = uimodel::TrackColumnState{
          .field = rt::TrackField::Artist, .visibility = uimodel::TrackColumnVisibility::Hide};
        columnLayouts.updateLayout(rt::kAllTracksListId, {hideArtist});
        drainGtkEvents();

        auto& view = currentColumnView(host);
        CHECK(view.get_columns() != columnsPtr);
        auto const newArtistPtr = columnForField(view, rt::TrackField::Artist);
        REQUIRE(newArtistPtr != nullptr);
        CHECK_FALSE(newArtistPtr->get_visible());
        checkSortArrow(host, rt::TrackField::TrackNumber, Gtk::SortType::ASCENDING);
      }
    }

    SECTION("a pending sort request belongs to its column view generation and runtime view")
    {
      auto const otherListId = ao::test::requireValue(
        runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Other page"})));
      auto const otherViewId = ao::test::requireValue(runtime.workspace().navigate({.target = otherListId}));
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      auto const original = runtime.views().trackListState(viewId).presentation;
      host.rebuild(cache);
      drainGtkEvents();

      auto* const entry = host.find(viewId);
      REQUIRE(entry != nullptr);
      auto* const view = findWidget<Gtk::ColumnView>(*entry->pagePtr);
      REQUIRE(view != nullptr);
      auto const tagsPtr = columnForField(*view, rt::TrackField::Tags);
      REQUIRE(tagsPtr != nullptr);
      CHECK(tagsPtr->get_sorter() == nullptr);
      auto const titlePtr = columnForField(*view, rt::TrackField::Title);
      REQUIRE(titlePtr != nullptr);
      REQUIRE(titlePtr->get_sorter() != nullptr);

      view->sort_by_column(titlePtr, Gtk::SortType::ASCENDING);
      REQUIRE(runtime.views().trackListState(viewId).presentation == original);
      auto expected = original;
      expected.optColumnSort = rt::TrackColumnSort{.column = rt::TrackField::Title, .ascending = true};

      SECTION("late delivery of an already rendered presentation preserves the queued sort")
      {
        auto const columnsPtr = view->get_columns();
        // A replacement projection can render the live spec before its posted
        // presentation event arrives. Deliver that same spec before the click's idle handler.
        entry->pagePtr->applyPresentation(original);
        drainGtkEvents();

        auto& currentView = currentColumnView(host);
        CHECK(currentView.get_columns() == columnsPtr);
        CHECK(columnForField(currentView, rt::TrackField::Title) == titlePtr);
        CHECK(runtime.views().trackListState(viewId).presentation == expected);
      }

      SECTION("a retired generation cancels its sort")
      {
        host.rebuild(cache);
        drainGtkEvents();
        CHECK(runtime.views().trackListState(viewId).presentation == original);
      }

      SECTION("changing focus does not retarget a sort")
      {
        auto const otherPresentation = runtime.views().trackListState(otherViewId).presentation;
        REQUIRE(runtime.workspace().focusView(otherViewId));
        drainGtkEvents();
        CHECK(runtime.views().trackListState(viewId).presentation == expected);
        CHECK(runtime.views().trackListState(otherViewId).presentation == otherPresentation);
        CHECK(runtime.workspace().snapshot().activeViewId == otherViewId);
      }
    }

    SECTION("authored columns stay authoritative when a filter replaces the projection")
    {
      auto const trackId = addRuntimeTrack(fixture.owner(), {.title = "Authored columns track", .album = "Album A"});
      auto const showAlbum =
        uimodel::TrackColumnState{.field = rt::TrackField::Album, .visibility = uimodel::TrackColumnVisibility::Show};
      columnLayouts.restore({{rt::kAllTracksListId, {showAlbum}}}, {});
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = rt::kAllTracksListId,
        .optPresentation =
          rt::NavigationPresentation{
            .spec = {.presetId = rt::CustomPresetId{1},
                     .groupBy = rt::TrackGroupKey::Album,
                     .visibleFields = {rt::TrackField::Title}},
          },
      }));
      host.rebuild(cache);
      drainGtkEvents();

      auto const* const entry = host.find(viewId);
      REQUIRE(entry);
      REQUIRE(entry->pagePtr);
      auto const isAlbumVisible = [entry]
      {
        auto* const view = findWidget<Gtk::ColumnView>(*entry->pagePtr);
        REQUIRE(view);
        auto const columnPtr = columnForField(*view, rt::TrackField::Album);
        REQUIRE(columnPtr != nullptr);
        return columnPtr->get_visible();
      };
      auto const initialProjectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
      REQUIRE(runtime.views().trackListState(viewId).presentation.redundantFields.empty());
      REQUIRE(std::ranges::contains(initialProjectionPtr->presentation().redundantFields, rt::TrackField::Album));
      CHECK_FALSE(isAlbumVisible());
      auto const originalColumnsPtr = currentColumnView(host).get_columns();

      REQUIRE(runtime.views().setFilter(viewId, R"($title = "Authored columns track")"));
      drainGtkEvents();
      auto const filteredProjectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
      REQUIRE(filteredProjectionPtr != initialProjectionPtr);
      REQUIRE(entry->pagePtr->projection() == filteredProjectionPtr.get());
      CHECK(currentColumnView(host).get_columns() != originalColumnsPtr);
      REQUIRE(filteredProjectionPtr->indexOf(trackId));
      REQUIRE(std::ranges::contains(filteredProjectionPtr->presentation().redundantFields, rt::TrackField::Album));
      CHECK_FALSE(isAlbumVisible());

      auto presentation = runtime.views().trackListState(viewId).presentation;
      presentation.redundantFields = {rt::TrackField::Album};
      REQUIRE(runtime.workspace().setActivePresentation(presentation, {.recordHistory = false}));
      drainGtkEvents();
      CHECK(isAlbumVisible());
    }

    SECTION("rebuild restores the runtime selection into the new page generation")
    {
      auto const trackId = addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Restored selection"});
      runtime.sources().reloadAllTracks();
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));

      REQUIRE(runtime.views().setSelection(viewId, {trackId}));

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.find(viewId);
      REQUIRE(context != nullptr);
      REQUIRE(context->pagePtr != nullptr);
      CHECK(context->pagePtr->selectionController().selectedTrackIds() == std::vector<TrackId>{trackId});
      CHECK(runtime.views().trackListState(viewId).selection == std::vector<TrackId>{trackId});
    }

    SECTION("rebuild restores mixed selections without publishing or changing workspace focus")
    {
      auto const firstTrackId = addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "A restored track"});
      auto const secondTrackId =
        addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Z restored track"});
      auto const absentTrackId = TrackId{9999};
      runtime.sources().reloadAllTracks();
      auto const listId = ao::test::requireValue(
        runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Other page"})));
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      REQUIRE(firstViewId != secondViewId);
      auto const storedSelection = std::vector{firstTrackId, absentTrackId, secondTrackId};
      REQUIRE(runtime.views().setSelection(firstViewId, storedSelection));
      REQUIRE(runtime.views().setSelection(secondViewId, {secondTrackId}));
      REQUIRE(runtime.workspace().focusView(firstViewId));
      host.rebuild(cache);
      drainGtkEvents();

      REQUIRE(runtime.workspace().focusView(firstViewId));
      drainGtkEvents();
      auto const workspaceBefore = runtime.workspace().snapshot();
      std::size_t selectionChangeCount = 0;
      std::size_t workspaceChangeCount = 0;
      auto selectionSubscription =
        runtime.views().onSelectionChanged([&](rt::ViewService::SelectionChanged const&) { ++selectionChangeCount; });
      auto workspaceSubscription =
        runtime.workspace().onChanged([&](rt::WorkspaceChanged const&) { ++workspaceChangeCount; });

      host.rebuild(cache);
      drainGtkEvents();

      auto const* const firstPage = host.find(firstViewId);
      auto const* const secondPage = host.find(secondViewId);
      REQUIRE(firstPage != nullptr);
      REQUIRE(secondPage != nullptr);
      CHECK(firstPage->pagePtr->selectionController().selectedTrackIds() ==
            std::vector<TrackId>{firstTrackId, secondTrackId});
      CHECK(secondPage->pagePtr->selectionController().selectedTrackIds() == std::vector<TrackId>{secondTrackId});
      CHECK(runtime.views().trackListState(firstViewId).selection == storedSelection);
      CHECK(selectionChangeCount == 0);
      CHECK(workspaceChangeCount == 0);
      CHECK(runtime.workspace().snapshot().activeViewId == workspaceBefore.activeViewId);
      CHECK(runtime.workspace().snapshot().revision == workspaceBefore.revision);
    }

    SECTION("title edits preserve selected track IDs when All Tracks reorders")
    {
      auto const editedTrackId = addRuntimeTrack(fixture.owner(), {.title = "Song A"});
      addRuntimeTrack(fixture.owner(), {.title = "Song B"});
      auto const neighborTrackId = addRuntimeTrack(fixture.owner(), {.title = "Song C"});
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
        .target = rt::GlobalViewKind::AllTracks,
        .optPresentation = rt::NavigationPresentation{.spec = {.sortBy = {{.field = rt::TrackSortField::Title}}}},
      }));
      auto const storedSelection = std::vector{editedTrackId, neighborTrackId};
      REQUIRE(runtime.views().setSelection(viewId, storedSelection));
      host.rebuild(cache);
      drainGtkEvents();

      auto const* const entry = host.find(viewId);
      REQUIRE(entry != nullptr);
      REQUIRE(entry->pagePtr);
      auto const& selection = entry->pagePtr->selectionController();
      REQUIRE(selection.selectedTrackIds() == storedSelection);

      std::size_t selectionChangeCount = 0;
      auto selectionSubscription =
        runtime.views().onSelectionChanged([&](rt::ViewService::SelectionChanged const&) { ++selectionChangeCount; });

      updateRuntimeTrack(fixture.owner(), editedTrackId, [](library::test::TrackSpec& spec) { spec.title = "Song D"; });
      drainGtkEvents();

      auto const* const projection = entry->pagePtr->projection();
      REQUIRE(projection != nullptr);
      CHECK(projection->indexOf(editedTrackId) == 2);
      CHECK(selection.selectedTrackIds() == std::vector{neighborTrackId, editedTrackId});
      CHECK(runtime.views().trackListState(viewId).selection == storedSelection);
      CHECK(selectionChangeCount == 0);
    }

    SECTION("rebuild tolerates a selection naming a track absent from the projection")
    {
      auto const absentTrackId = TrackId{9999};
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));

      REQUIRE(runtime.views().setSelection(viewId, {absentTrackId}));

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.find(viewId);
      REQUIRE(context != nullptr);
      REQUIRE(context->pagePtr != nullptr);
      CHECK(context->pagePtr->selectionController().selectedTrackIds().empty());
      CHECK(runtime.views().trackListState(viewId).selection == std::vector<TrackId>{absentTrackId});
    }

    SECTION("group placeholder style reaches future and existing page generations")
    {
      host.setGroupCoverPlaceholderStyle(uimodel::CoverArtPlaceholderStyle::Vinyl);
      REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.currentVisible();
      REQUIRE(context != nullptr);
      REQUIRE(context->pagePtr != nullptr);
      CHECK(context->pagePtr->groupCoverPlaceholderStyle() == uimodel::CoverArtPlaceholderStyle::Vinyl);

      host.setGroupCoverPlaceholderStyle(uimodel::CoverArtPlaceholderStyle::Soul);
      CHECK(context->pagePtr->groupCoverPlaceholderStyle() == uimodel::CoverArtPlaceholderStyle::Soul);
    }

    SECTION("focus and close retire the exact page generation")
    {
      auto const listId = ao::test::requireValue(
        runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Second page"})));
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      REQUIRE(firstViewId != secondViewId);

      host.rebuild(cache);
      drainGtkEvents();
      REQUIRE(host.find(firstViewId) != nullptr);
      REQUIRE(host.find(secondViewId) != nullptr);
      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == secondViewId);

      REQUIRE(runtime.workspace().focusView(firstViewId));
      drainGtkEvents();
      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == firstViewId);

      REQUIRE(runtime.workspace().closeView(firstViewId));
      drainGtkEvents();
      CHECK(host.find(firstViewId) == nullptr);
      REQUIRE(host.find(secondViewId) != nullptr);
      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == secondViewId);
      CHECK(runtime.workspace().snapshot().activeViewId == secondViewId);
    }

    SECTION("track activation starts from the owning view identity")
    {
      rt::test::addReadyAudioProvider(runtime);
      auto const trackId = addRuntimeTrack(
        fixture.owner(),
        library::test::TrackSpec{
          .title = "Activated", .uri = audio::test::requireAudioFixture("basic_metadata.flac").string()});
      runtime.sources().reloadAllTracks();
      REQUIRE(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));
      auto const viewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(viewId != rt::kInvalidViewId);

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.currentVisible();
      REQUIRE(context != nullptr);
      context->pagePtr->signalTrackActivated().emit(trackId);
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));

      auto const snapshot = runtime.playback().snapshot();
      CHECK(snapshot.succession.currentTrackId == trackId);
      CHECK(snapshot.succession.sourceListId == rt::kAllTracksListId);
      CHECK(snapshot.transport.nowPlaying.trackId == trackId);
    }

    SECTION("reveal playing track opens its missing source page and selects the restored row")
    {
      auto const trackId = restoreRevealTarget(fixture.owner());
      host.rebuild(cache);
      REQUIRE(host.currentVisible() == nullptr);

      runtime.playback().commands().revealPlayingTrack();
      drainGtkEvents();

      auto const* const page = host.currentVisible();
      REQUIRE(page != nullptr);
      CHECK(page->pagePtr->listId() == rt::kAllTracksListId);
      CHECK(page->pagePtr->selectionController().selectedTrackIds() == std::vector{trackId});
      CHECK(runtime.views().trackListState(page->viewId).selection == std::vector{trackId});
    }

    SECTION("reveal reconciles a stale runtime selection when the row is already selected")
    {
      auto const trackId = restoreRevealTarget(fixture.owner());
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      auto const storedSelection = std::vector{trackId, TrackId{9999}};
      REQUIRE(runtime.views().setSelection(viewId, storedSelection));
      host.rebuild(cache);
      drainGtkEvents();
      auto const* const page = host.currentVisible();
      REQUIRE(page != nullptr);
      REQUIRE(page->pagePtr->selectionController().selectedTrackIds() == std::vector{trackId});
      REQUIRE(runtime.views().trackListState(viewId).selection == storedSelection);

      SECTION("Playback source reveal")
      {
        runtime.playback().commands().revealPlayingTrack();
      }

      SECTION("Explicit track reveal")
      {
        runtime.playback().commands().revealTrack(trackId, viewId, rt::kAllTracksListId);
      }

      drainGtkEvents();

      CHECK(page->pagePtr->selectionController().selectedTrackIds() == std::vector{trackId});
      CHECK(runtime.views().trackListState(viewId).selection == std::vector{trackId});
    }

    SECTION("reveal selects the newly visible track before queued filter replacements arrive")
    {
      auto const trackId = restoreRevealTarget(fixture.owner());
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      REQUIRE(runtime.views().setFilter(viewId, R"($title = "Hidden")"));
      host.rebuild(cache);
      auto mount = GtkWindowFixture{};
      mount.mount(stack);
      mount.present();
      auto const* const page = host.currentVisible();
      REQUIRE(page != nullptr);
      REQUIRE(page->pagePtr->get_mapped());
      REQUIRE_FALSE(page->modelPtr->indexOf(trackId));

      // Leave both the superseded replacement and the final, unfiltered one queued.
      REQUIRE(runtime.views().setFilter(viewId, R"($title = "Another title")"));
      REQUIRE(runtime.views().setFilter(viewId, ""));
      runtime.playback().commands().revealPlayingTrack();
      CHECK(page->pagePtr->selectionController().selectedTrackIds() == std::vector{trackId});
      drainGtkEvents();

      REQUIRE(host.currentVisible() == page);
      CHECK(page->pagePtr->selectionController().selectedTrackIds() == std::vector{trackId});
      CHECK(runtime.views().trackListState(viewId).selection == std::vector{trackId});
    }

    SECTION("a hidden page's queued filter reset keeps the revealed page focused")
    {
      auto const trackId = restoreRevealTarget(fixture.owner());
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      host.rebuild(cache);
      drainGtkEvents();
      auto const* const oldPage = host.currentVisible();
      REQUIRE(oldPage != nullptr);
      oldPage->pagePtr->selectionController().selectTrack(trackId);
      REQUIRE(runtime.views().trackListState(viewId).selection == std::vector{trackId});

      auto const* const filter = R"($title = "Hidden")";
      REQUIRE(runtime.views().setFilter(viewId, filter));
      runtime.playback().commands().revealPlayingTrack();
      auto const newView = runtime.workspace().snapshot().activeViewId;
      REQUIRE(newView != viewId);
      drainGtkEvents();

      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == newView);
      CHECK(runtime.workspace().snapshot().activeViewId == newView);
      CHECK(runtime.views().trackListState(viewId).filterExpression == filter);
      CHECK(runtime.views().trackListState(newView).selection == std::vector{trackId});
    }

    SECTION("reveal synchronizes a missing workspace page before selecting the track")
    {
      auto const trackId = addRuntimeTrack(fixture.owner(), library::test::TrackSpec{.title = "Reveal Target"});
      runtime.sources().reloadAllTracks();
      host.rebuild(cache);
      REQUIRE(host.currentVisible() == nullptr);

      auto const res = runtime.jumpToAlbum(trackId);

      REQUIRE(res);
      drainGtkEvents();
      auto const activeViewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(host.find(activeViewId) != nullptr);
      CHECK(runtime.views().trackListState(activeViewId).selection == std::vector<TrackId>{trackId});
    }
  }

  TEST_CASE("TrackPageHost - reversing a header sort at the top shows the new first row without selection")
  {
    requireOwnedGtkDisplay();
    constexpr std::int32_t kTrackCount = 512;
    constexpr std::int32_t kMovementCount = 23;
    auto ascendingFirst = kInvalidTrackId;
    auto descendingFirst = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          auto transaction = library::test::writeTransaction(musicLibrary);
                          REQUIRE(transaction.apply(
                            [&](library::LibraryWrite& write) -> Result<>
                            {
                              for (std::int32_t const index : std::views::iota(std::int32_t{0}, kTrackCount))
                              {
                                auto const id = library::test::addTrack(
                                  musicLibrary,
                                  write,
                                  {.title = std::format("Track {:06}", index),
                                   .uri = std::format("track-{}.flac", index),
                                   .movementNumber = static_cast<std::uint16_t>(1 + (index % kMovementCount))});
                                if (index == 0)
                                {
                                  ascendingFirst = id;
                                }
                                else if (index == kMovementCount - 1)
                                {
                                  descendingFirst = id;
                                }
                              }
                              return {};
                            }));
                          REQUIRE(transaction.commit());
                        }};
    auto& runtime = fixture.runtime();
    auto const& catalog = ao::test::englishMessageCatalog();
    auto cache = TrackRowCache{runtime.library(), catalog};
    auto stack = Gtk::Stack{};
    auto mount = GtkWindowFixture{};
    auto& window = mount.window();
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditController = TagEditController{window,
                                               runtime.async(),
                                               fixture.editing(),
                                               runtime.completion(),
                                               runtime.notifications(),
                                               runtime.textOrderingPolicy(),
                                               catalog,
                                               {},
                                               themeCoordinator};
    auto listNavigation = ListNavigationController{window, runtime, fixture.editing(), catalog, {}, themeCoordinator};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto host = TrackPageHost{stack,
                              runtime.async(),
                              fixture.editing(),
                              runtime.playback(),
                              runtime.views(),
                              runtime.workspace(),
                              tagEditController,
                              listNavigation,
                              columnLayouts,
                              catalog,
                              runtime.resourceBytes()};

    auto const presentation = rt::TrackPresentationSpec{
      .sortBy = {{.field = rt::TrackSortField::Movement}, {.field = rt::TrackSortField::Title}},
      .visibleFields = {rt::TrackField::Title, rt::TrackField::MovementNumber},
    };
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = rt::kAllTracksListId,
      .optPresentation = rt::NavigationPresentation{.spec = presentation},
    }));
    host.rebuild(cache);
    window.set_default_size(640, 320);
    mount.mount(stack);
    mount.present();
    auto pointer = GtkNativePointerFixture{stack};
    auto const* const entry = host.find(viewId);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->pagePtr->projection() != nullptr);

    clickHeader(stack, host, pointer, "Movement No.");
    checkSortArrow(host, rt::TrackField::MovementNumber, Gtk::SortType::ASCENDING);
    REQUIRE(entry->pagePtr->projection()->trackIdAt(0) == ascendingFirst);
    REQUIRE(tryPumpGtkEventsUntil([&] { return firstVisibleTrackTitle(currentColumnView(host)) == "Track 000000"; }));
    auto const ascendingAdjustmentPtr = currentColumnView(host).get_vadjustment();
    REQUIRE(ascendingAdjustmentPtr != nullptr);
    REQUIRE(ascendingAdjustmentPtr->get_page_size() > 0.0);
    REQUIRE(ascendingAdjustmentPtr->get_upper() - ascendingAdjustmentPtr->get_lower() >
            ascendingAdjustmentPtr->get_page_size());
    REQUIRE(ascendingAdjustmentPtr->get_value() == ascendingAdjustmentPtr->get_lower());
    REQUIRE(entry->pagePtr->selectionController().selectedTrackIds().empty());

    clickHeader(stack, host, pointer, "Movement No.");
    checkSortArrow(host, rt::TrackField::MovementNumber, Gtk::SortType::DESCENDING);
    REQUIRE(entry->pagePtr->projection()->trackIdAt(0) == descendingFirst);
    // Projection order and the native arrow can already be right while GTK
    // anchors the viewport to the old first track.
    auto visibleTitle = std::string{};
    std::ignore = tryPumpGtkEventsUntil(
      [&]
      {
        auto& view = currentColumnView(host);
        visibleTitle = firstVisibleTrackTitle(view);
        auto const adjustmentPtr = view.get_vadjustment();
        return visibleTitle == "Track 000022" && adjustmentPtr &&
               adjustmentPtr->get_value() == adjustmentPtr->get_lower();
      });

    CHECK(visibleTitle == "Track 000022");
    auto const descendingAdjustmentPtr = currentColumnView(host).get_vadjustment();
    REQUIRE(descendingAdjustmentPtr != nullptr);
    CHECK(descendingAdjustmentPtr->get_value() == descendingAdjustmentPtr->get_lower());
    CHECK(entry->pagePtr->selectionController().selectedTrackIds().empty());
  }
} // namespace ao::gtk::test
