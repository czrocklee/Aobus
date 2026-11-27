// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "image/CoverArtView.h"
#include "image/ImageCache.h"
#include "image/ResourceImageLoader.h"
#include "layout/LayoutConstants.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkNativeInputTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackListItem.h"
#include "track/TrackListModel.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gdk/gdk.h>
#include <giomm/listmodel.h>
#include <glib-object.h>
#include <glibmm/ustring.h>
#include <glibmm/value.h>
#include <gtkmm/box.h>
#include <gtkmm/columnview.h>
#include <gtkmm/columnviewcolumn.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/sorter.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addAlbumTrack(library::MusicLibrary& library, std::string const& album)
    {
      return library::test::addTrackWithUniqueFixtureUri(library,
                                                         library::test::TrackSpec{.title = "Track",
                                                                                  .artist = "Artist",
                                                                                  .album = album,
                                                                                  .albumArtist = "Album Artist",
                                                                                  .uri = "track.flac",
                                                                                  .year = 2023,
                                                                                  .trackNumber = 1,
                                                                                  .duration = std::chrono::minutes{3}});
    }

    std::vector<TrackId> seedLargeProjection(library::MusicLibrary& library, std::size_t count)
    {
      auto transaction = library::test::writeTransaction(library);
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(count);

      REQUIRE(transaction.apply(
        [&](library::LibraryWrite& write) -> Result<>
        {
          auto writer = write.tracks();

          for (std::size_t const index : std::views::iota(std::size_t{0}, count))
          {
            auto builder = library::TrackBuilder::makeEmpty();
            auto const spec = library::test::TrackSpec{
              .title = std::format("Track {:05}", index),
              .artist = std::format("Artist {:03}", index % 100),
              .album = std::format("Album {:04}", index % 1000),
              .albumArtist = std::format("Album Artist {:03}", index % 100),
              .uri = std::format("music/track_{}.flac", index),
              .duration = std::chrono::minutes{3},
            };
            library::test::applyTrackSpec(builder, spec);

            trackIds.push_back(
              ao::test::requireValue(writer.create(builder, library::FileManifestBuilder::makeEmpty())));
          }

          return {};
        }));

      REQUIRE(transaction.commit());
      return trackIds;
    }
  } // namespace

  TEST_CASE("TrackViewPage - exposes list identity and status message state")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{[](library::MusicLibrary& musicLibrary)
                                     { std::ignore = addAlbumTrack(musicLibrary, "Album"); }};
    auto& runtime = fixture.runtime();
    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto cache = TrackRowCache{runtime.library(), textCatalog};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto window = Gtk::Window{};
    auto modelPtr = TrackListModel::create(cache);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              textCatalog,
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader};
    window.set_child(page);

    CHECK(page.listId() == rt::kAllTracksListId);
    CHECK(page.projection() == nullptr);
    page.setStatusMessage("Loading...");
    auto* const label = findLabelByText(page, "Loading...");
    REQUIRE(label != nullptr);
    CHECK(label->get_visible());
    page.clearStatusMessage();
    CHECK_FALSE(label->get_visible());
  }

  TEST_CASE("TrackViewPage - a queued header click cannot sort a replacement presentation")
  {
    requireOwnedGtkDisplay();
    auto window = GtkWindowFixture{};
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const original = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Songs).spec;
    auto const replacement = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums).spec;
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = rt::kAllTracksListId,
      .optPresentation = rt::NavigationPresentation{.spec = original},
    }));
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto cache = TrackRowCache{runtime.library(), textCatalog};
    auto modelPtr = TrackListModel::create(cache);
    modelPtr->bindProjection(ao::test::requireValue(runtime.views().findTrackListProjection(viewId)));
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              textCatalog,
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader,
                              original,
                              viewId};
    window.window().set_default_size(850, 400);
    window.mount(page);
    window.present();

    auto* const view = findWidget<Gtk::ColumnView>(page);
    REQUIRE(view != nullptr);
    auto const sorterPtr = view->get_sorter();
    REQUIRE(sorterPtr != nullptr);
    auto specAtClick = replacement;
    bool committed = false;
    auto const replaceOnClick = sigc::scoped_connection{sorterPtr->signal_changed().connect(
      [&](Gtk::Sorter::Change)
      {
        specAtClick = runtime.views().trackListState(viewId).presentation;
        committed = runtime.workspace().setActivePresentation(replacement).has_value();
      })};

    // The sorter handles the real click, then this observer commits Albums on
    // the same dispatch stack. With no host attached, the Songs generation is
    // still alive at idle; the test does not depend on publication timing.
    double x = 0;
    double y = 0;
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        auto* const header = findLabelByText(page, "Title");
        return header && header->get_mapped() && header->get_width() > 0 && header->get_height() > 0 &&
               header->translate_coordinates(page, header->get_width() / 2.0, header->get_height() / 2.0, x, y);
      }));
    auto pointer = GtkNativePointerFixture{page};
    pointer.movePointerTo(x, y);
    pointer.press();
    pointer.release();
    drainGtkEvents();

    REQUIRE(committed);
    CHECK(specAtClick == original);
    CHECK(runtime.workspace().snapshot().activeViewId == viewId);
    CHECK(runtime.views().trackListState(viewId).presentation == replacement);
    window.unmount();
  }

  TEST_CASE("TrackViewPage - grouped headers render localized cover placeholders")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto albumTrackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     { albumTrackId = addAlbumTrack(musicLibrary, "Album"); }};
    auto& runtime = fixture.runtime();
    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto cache = TrackRowCache{runtime.library(), textCatalog};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto window = Gtk::Window{};

    auto modelPtr = TrackListModel::create(cache);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};

    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              textCatalog,
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader};
    window.set_child(page);

    SECTION("album grouped section header reserves a fixed cover slot")
    {
      auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
      sourcePtr->addInitial(albumTrackId);

      auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
        runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
      auto presentation = rt::TrackPresentationSpec{.groupBy = rt::TrackGroupKey::Album};
      projectionPtr->setPresentation(presentation);
      modelPtr->bindProjection(projectionPtr);
      page.applyPresentation(projectionPtr->presentation());

      window.set_default_size(600, 320);
      window.set_visible(true);
      drainGtkEvents();

      auto* const coverSlot = findWidgetByClass<Gtk::Widget>(page, "ao-track-section-cover");
      REQUIRE(coverSlot != nullptr);
      CHECK(coverSlot->get_visible());
      CHECK(findLabelByText(page, "• (1 Titel)") != nullptr);
      auto* const coverArt = dynamic_cast<CoverArtView*>(coverSlot->get_first_child());
      REQUIRE(coverArt != nullptr);
      CHECK(coverArt->get_width() >= layout::kSectionCoverLogicalSize);
      CHECK(coverArt->get_height() >= layout::kSectionCoverLogicalSize);
      CHECK(coverArt->isShowingPlaceholder());
      CHECK(coverArt->placeholderPresentation().monogram == "A");

      std::int32_t minSize = {};
      std::int32_t natSizeHoriz = {};
      std::int32_t natSizeVert = {};
      std::int32_t minBaseline = {};
      std::int32_t natBaseline = {};
      coverSlot->measure(Gtk::Orientation::HORIZONTAL, -1, minSize, natSizeHoriz, minBaseline, natBaseline);
      coverSlot->measure(Gtk::Orientation::VERTICAL, -1, minSize, natSizeVert, minBaseline, natBaseline);

      CHECK(natSizeHoriz == natSizeVert);
      CHECK(natSizeHoriz >= layout::kSectionCoverLogicalSize);
      CHECK(natSizeHoriz <= layout::kSectionCoverLogicalSize + 2);
    }

    SECTION("non-album grouped section header displays its semantic monogram")
    {
      auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
      sourcePtr->addInitial(albumTrackId);

      auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
        runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
      auto presentation = rt::TrackPresentationSpec{
        .groupBy = rt::TrackGroupKey::Year,
        .sortBy = {{.field = rt::TrackSortField::Year}},
      };
      projectionPtr->setPresentation(presentation);
      modelPtr->bindProjection(projectionPtr);
      page.applyPresentation(projectionPtr->presentation());

      window.set_default_size(600, 320);
      window.set_visible(true);
      drainGtkEvents();

      auto* const coverSlot = findWidgetByClass<Gtk::Widget>(page, "ao-track-section-cover");
      REQUIRE(coverSlot != nullptr);
      CHECK(coverSlot->get_visible());
      auto* const coverArt = dynamic_cast<CoverArtView*>(coverSlot->get_first_child());
      REQUIRE(coverArt != nullptr);
      CHECK(coverArt->isShowingPlaceholder());
      CHECK(coverArt->placeholderPresentation().monogram == "23");
    }
  }

  TEST_CASE("TrackViewPage - honors restored visibility for an Album column suppressed by grouping")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto cache = TrackRowCache{runtime.library(), textCatalog};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto const& presentation = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums).spec;
    auto const showAlbum = uimodel::TrackColumnState{
      .field = rt::TrackField::Album, .weight = 1.75, .visibility = uimodel::TrackColumnVisibility::Show};
    columnLayouts.restore({{rt::kAllTracksListId, {showAlbum}}}, {});

    auto page = TrackViewPage{rt::kAllTracksListId,
                              TrackListModel::create(cache),
                              columnLayouts,
                              textCatalog,
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader,
                              presentation};
    auto const isAlbumVisible = [&page]
    {
      auto* const columnView = findWidget<Gtk::ColumnView>(page);
      REQUIRE(columnView);
      auto const columnsPtr = columnView->get_columns();
      REQUIRE(columnsPtr);

      for (std::uint32_t index = 0; index < columnsPtr->get_n_items(); ++index)
      {
        auto const columnPtr = std::dynamic_pointer_cast<Gtk::ColumnViewColumn>(columnsPtr->get_object(index));

        if (columnPtr && columnPtr->get_id().raw() == "album")
        {
          return columnPtr->get_visible();
        }
      }

      FAIL("The track page did not register an Album column");
      return false;
    };

    CHECK(isAlbumVisible());
    columnLayouts.updateLayout(rt::kAllTracksListId, {});
    page.applyPresentation(presentation);
    CHECK_FALSE(isAlbumVisible());

    columnLayouts.updateLayout(rt::kAllTracksListId, {showAlbum});
    page.applyPresentation(presentation);
    CHECK(isAlbumVisible());
  }

  TEST_CASE("TrackViewPage - large projections materialize only the GTK prefetch window")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    constexpr std::size_t kTrackCount = 10000;
    constexpr std::size_t kMaximumPrefetchedRows = kTrackCount / 10;
    auto trackIds = std::vector<TrackId>{};
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     { trackIds = seedLargeProjection(musicLibrary, kTrackCount); }};
    auto& runtime = fixture.runtime();
    REQUIRE(trackIds.size() == kTrackCount);

    auto sourcePtr = rt::test::makeMutableTrackSource(trackIds);
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};

    auto materializedRowsForPage = [&]
    {
      auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
      auto modelPtr = TrackListModel::create(rowCache);
      modelPtr->bindProjection(projectionPtr);
      auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
      auto page = TrackViewPage{rt::kAllTracksListId,
                                modelPtr,
                                columnLayouts,
                                ao::test::englishMessageCatalog(),
                                runtime.async(),
                                fixture.editing(),
                                runtime.views(),
                                thumbnailLoader};
      auto window = Gtk::Window{};
      window.set_child(page);
      window.set_default_size(600, 320);
      window.set_visible(true);
      drainGtkEvents();

      auto const materializedRows = rowCache.cachedRowCount();
      window.unset_child();
      drainGtkEvents();
      return materializedRows;
    };

    auto const ungroupedRows = materializedRowsForPage();
    CHECK(ungroupedRows > 0);
    CHECK(ungroupedRows < kMaximumPrefetchedRows);

    projectionPtr->setPresentation(rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album}},
    });

    auto const groupedRows = materializedRowsForPage();
    CHECK(groupedRows > 0);
    CHECK(groupedRows < kMaximumPrefetchedRows);
  }

  TEST_CASE("TrackViewPage - layout generations load paths once and keep demand local")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const firstId = addRuntimeTrack(fixture.owner(), {.title = "First library track"});
    auto const secondId = addRuntimeTrack(fixture.owner(), {.title = "Second library track"});
    auto const thirdId = addRuntimeTrack(fixture.owner(), {.title = "Third library track"});
    auto const optFirstUri = runtime.library().snapshot().trackUri(firstId);
    auto const optSecondUri = runtime.library().snapshot().trackUri(secondId);
    REQUIRE(optFirstUri);
    REQUIRE(optSecondUri);
    auto const firstPath = (runtime.musicRoot() / *optFirstUri).string();
    auto const secondPath = (runtime.musicRoot() / *optSecondUri).string();
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const cache = TrackRowCache{runtime.library(), textCatalog};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto const& technical = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Technical).spec;
    auto const hiddenPath =
      uimodel::TrackColumnState{.field = rt::TrackField::FilePath, .visibility = uimodel::TrackColumnVisibility::Hide};
    constexpr auto kShownList = rt::kAllTracksListId;
    auto const hiddenList = ListId{198};
    columnLayouts.updateLayout(hiddenList, {hiddenPath});

    auto const makeModel = [&](std::shared_ptr<rt::test::MutableTrackSource> const& sourcePtr)
    {
      auto modelPtr = TrackListModel::create(cache);
      modelPtr->bindProjection(runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr}));
      return modelPtr;
    };
    auto const shownSourcePtr = rt::test::makeMutableTrackSource(std::array{firstId});
    auto const hiddenSourcePtr = rt::test::makeMutableTrackSource(std::array{firstId, secondId});
    auto shownPage = TrackViewPage{kShownList,
                                   makeModel(shownSourcePtr),
                                   columnLayouts,
                                   textCatalog,
                                   runtime.async(),
                                   fixture.editing(),
                                   runtime.views(),
                                   thumbnailLoader,
                                   technical};
    auto shownWindow = Gtk::Window{};
    shownWindow.set_default_size(800, 300);
    shownWindow.set_child(shownPage);
    shownWindow.set_visible(true);
    REQUIRE(tryPumpGtkEventsUntil([&] { return findLabelByText(shownPage, firstPath) != nullptr; }));
    CHECK(findLabelByText(shownPage, "First library track") != nullptr);
    CHECK(cache.rowLoadCount() == 1);
    auto const firstRowPtr = cache.trackRow(firstId);
    REQUIRE(firstRowPtr);

    auto hiddenPage = TrackViewPage{hiddenList,
                                    makeModel(hiddenSourcePtr),
                                    columnLayouts,
                                    textCatalog,
                                    runtime.async(),
                                    fixture.editing(),
                                    runtime.views(),
                                    thumbnailLoader,
                                    technical};
    auto hiddenWindow = Gtk::Window{};
    hiddenWindow.set_default_size(800, 300);
    hiddenWindow.set_child(hiddenPage);
    hiddenWindow.set_visible(true);
    REQUIRE(tryPumpGtkEventsUntil([&] { return findLabelByText(hiddenPage, "Second library track") != nullptr; }));
    CHECK(findLabelByText(hiddenPage, firstPath) == nullptr);
    CHECK(cache.rowLoadCount() == 2);
    auto const secondRowPtr = cache.trackRow(secondId);
    REQUIRE(secondRowPtr);
    CHECK(secondRowPtr->fieldText(rt::TrackField::FilePath).empty());
    CHECK(firstRowPtr->fieldText(rt::TrackField::FilePath) == firstPath);

    columnLayouts.updateLayout(hiddenList, {});
    hiddenPage.applyPresentation(technical);
    REQUIRE(tryPumpGtkEventsUntil([&] { return findLabelByText(hiddenPage, secondPath) != nullptr; }));
    CHECK(cache.trackRow(secondId) == secondRowPtr);
    CHECK(cache.rowLoadCount() == 3);
    CHECK_FALSE(secondRowPtr->signalFilePathChanged().empty());

    columnLayouts.updateLayout(hiddenList, {hiddenPath});
    hiddenPage.applyPresentation(technical);
    hiddenSourcePtr->reset(std::array{firstId, secondId, thirdId});
    REQUIRE(tryPumpGtkEventsUntil([&] { return findLabelByText(hiddenPage, "Third library track") != nullptr; }));
    CHECK(findLabelByText(hiddenPage, secondPath) == nullptr);
    CHECK(secondRowPtr->signalFilePathChanged().empty());
    CHECK(cache.rowLoadCount() == 4);
    auto const thirdRowPtr = cache.trackRow(thirdId);
    REQUIRE(thirdRowPtr);
    CHECK(thirdRowPtr->fieldText(rt::TrackField::FilePath).empty());
    CHECK(findLabelByText(shownPage, firstPath) != nullptr);
    CHECK_FALSE(firstRowPtr->signalFilePathChanged().empty());

    shownWindow.unset_child();
    hiddenWindow.unset_child();
    drainGtkEvents();
  }

  TEST_CASE("TrackViewPage - drag handle follows the shared List order capability")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{[](library::MusicLibrary& musicLibrary)
                                     { std::ignore = addAlbumTrack(musicLibrary, "Album"); }};
    auto& runtime = fixture.runtime();
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Ordered"})));
    auto const& manual = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::ListOrder).spec;
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate(rt::NavigationRequest{
      .target = rt::FilteredListTarget{.listId = listId, .filterExpression = ""},
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = manual,
        },
    }));
    auto projectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(cache);
    modelPtr->bindProjection(projectionPtr);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto page = TrackViewPage{listId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader,
                              manual,
                              viewId};
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(page);
    windowFixture.present();

    auto const presentableFieldCount = static_cast<std::size_t>(
      std::ranges::count_if(rt::trackFieldDefinitions(), &rt::TrackFieldDefinition::presentable));
    auto const currentColumns = [&page]
    {
      auto* const columnView = findWidget<Gtk::ColumnView>(page);
      REQUIRE(columnView != nullptr);
      auto const columnsPtr = columnView->get_columns();
      REQUIRE(columnsPtr);
      return columnsPtr;
    };
    auto checkCellAlignment = [&page]
    {
      REQUIRE(tryPumpGtkEventsUntil(
        [&page]
        {
          auto* const label = findWidgetByClass<Gtk::Label>(page, "ao-track-title-cell");
          return label != nullptr && label->get_text() == "Track";
        }));
      auto* const titleLabel = findWidgetByClass<Gtk::Label>(page, "ao-track-title-cell");
      auto* const durationLabel = findLabelByText(page, "3:00");
      REQUIRE(titleLabel != nullptr);
      REQUIRE(durationLabel != nullptr);
      CHECK(titleLabel->get_text() == "Track");
      CHECK(titleLabel->get_halign() == Gtk::Align::START);
      CHECK(titleLabel->get_xalign() == 0.0F);
      CHECK(durationLabel->get_halign() == Gtk::Align::END);
      CHECK(durationLabel->get_xalign() == 1.0F);
    };
    auto const manualColumnsPtr = currentColumns();
    CHECK(manualColumnsPtr->get_n_items() == presentableFieldCount + 1);
    checkCellAlignment();
    CHECK(page.hasOrderDragHandle());
    auto* const dragHandle = findWidgetByClass<Gtk::Box>(page, "ao-order-drag-handle");
    REQUIRE(dragHandle != nullptr);
    CHECK(dragHandle->get_tooltip_text() == "Drag to rearrange tracks in Manual Order");
    CHECK(hasAccessibleLabel(*dragHandle, "Rearrange track"));

    REQUIRE(runtime.views().setColumnSort(viewId, {.column = rt::TrackField::Title}));
    auto const sorted = runtime.views().trackListState(viewId).presentation;
    REQUIRE(sorted.isPreset(rt::BuiltinPreset::ListOrder));
    page.applyPresentation(sorted);
    CHECK_FALSE(page.hasOrderDragHandle());
    CHECK_FALSE(page.orderCapabilities().canAuthorOrder);
    auto const sortedColumnsPtr = currentColumns();
    CHECK(sortedColumnsPtr != manualColumnsPtr);
    CHECK(sortedColumnsPtr->get_n_items() == presentableFieldCount);

    REQUIRE(runtime.workspace().setActivePresentation(rt::defaultTrackPresentationSpec(), {.recordHistory = false}));
    page.applyPresentation(rt::defaultTrackPresentationSpec());
    CHECK_FALSE(page.hasOrderDragHandle());
    auto const defaultColumnsPtr = currentColumns();
    CHECK(defaultColumnsPtr->get_n_items() == presentableFieldCount);
    checkCellAlignment();

    REQUIRE(runtime.workspace().setActivePresentation(manual, {.recordHistory = false}));
    page.applyPresentation(manual);
    CHECK(page.hasOrderDragHandle());
    auto const restoredColumnsPtr = currentColumns();
    CHECK(restoredColumnsPtr != defaultColumnsPtr);
    CHECK(restoredColumnsPtr->get_n_items() == presentableFieldCount + 1);
    checkCellAlignment();

    REQUIRE(runtime.views().setFilter(viewId, "true"));
    page.applyPresentation(manual);
    CHECK_FALSE(page.hasOrderDragHandle());

    REQUIRE(runtime.views().setFilter(viewId, ""));
    page.refreshOrderCapabilities();
    CHECK(page.hasOrderDragHandle());

    REQUIRE(runtime.views().setFilter(viewId, "("));
    page.refreshOrderCapabilities();
    CHECK_FALSE(page.hasOrderDragHandle());
    CHECK(page.orderCapabilities().disabledReason ==
          "Fix the List or quick-filter expression before changing its order.");

    REQUIRE(runtime.workspace().closeView(viewId));
    page.refreshOrderCapabilities();
    CHECK_FALSE(page.hasOrderDragHandle());
    CHECK(page.orderCapabilities().disabledReason == "This List is no longer available.");
  }

  TEST_CASE("TrackViewPage - dropping a drag handle preserves the order submission")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{[](library::MusicLibrary& musicLibrary)
                                     {
                                       std::ignore = addAlbumTrack(musicLibrary, "First");
                                       std::ignore = addAlbumTrack(musicLibrary, "Second");
                                     }};
    auto& runtime = fixture.runtime();
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Ordered"})));
    auto const& manual = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::ListOrder).spec;
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate(rt::NavigationRequest{
      .target = rt::FilteredListTarget{.listId = listId, .filterExpression = ""},
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = manual,
        },
    }));
    auto const sourceIdsRes = runtime.views().listSourceTrackIds(viewId);
    REQUIRE(sourceIdsRes);
    REQUIRE(sourceIdsRes->size() == 2);
    auto const& initialTrackIds = *sourceIdsRes;
    auto projectionPtr = ao::test::requireValue(runtime.views().findTrackListProjection(viewId));
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(cache);
    modelPtr->bindProjection(projectionPtr);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto page = TrackViewPage{listId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader,
                              manual,
                              viewId};
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(page);
    windowFixture.present();

    Gtk::Box* sourceHandle = nullptr;
    Gtk::Box* targetHandle = nullptr;

    for (auto* const handle : collectAll<Gtk::Box>(page))
    {
      if (!hasCssClass(*handle, "ao-order-drag-handle"))
      {
        continue;
      }

      auto const rawId = GPOINTER_TO_UINT(::g_object_get_data(G_OBJECT(handle->gobj()), kBoundTrackIdDataKey));
      auto const trackId = TrackId{static_cast<std::uint32_t>(rawId)};

      if (trackId == initialTrackIds[0])
      {
        sourceHandle = handle;
      }
      else if (trackId == initialTrackIds[1])
      {
        targetHandle = handle;
      }
    }

    REQUIRE(sourceHandle != nullptr);
    REQUIRE(targetHandle != nullptr);
    auto const dragSourcePtr = findController<Gtk::DragSource>(*sourceHandle);
    auto const dropTargetPtr = findController<Gtk::DropTarget>(*targetHandle);
    REQUIRE(dragSourcePtr);
    REQUIRE(dropTargetPtr);

    GdkContentProvider* rawProvider = nullptr;
    ::g_signal_emit_by_name(dragSourcePtr->gobj(), "prepare", 1.0, 1.0, &rawProvider);
    REQUIRE(rawProvider != nullptr);
    auto providerPtr = utility::makeUniquePtr<::g_object_unref>(rawProvider);
    auto dropValue = Glib::Value<std::string>{};
    dropValue.init(Glib::Value<std::string>::value_type());
    GError* rawError = nullptr;
    auto const valueLoaded = ::gdk_content_provider_get_value(providerPtr.get(), dropValue.gobj(), &rawError) != 0;
    auto errorPtr = utility::makeUniquePtr<::g_error_free>(rawError);
    CHECK(errorPtr == nullptr);
    REQUIRE(valueLoaded);

    gboolean accepted = FALSE;
    auto const targetBottom = static_cast<double>(std::max(1, targetHandle->get_height()));
    ::g_signal_emit_by_name(dropTargetPtr->gobj(), "drop", dropValue.gobj(), 1.0, targetBottom, &accepted);
    REQUIRE(accepted != 0);

    auto expectedTrackIds = initialTrackIds;
    std::ranges::reverse(expectedTrackIds);
    auto observedTrackIds = std::vector<TrackId>{};
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        auto const reorderedRes = runtime.views().listSourceTrackIds(viewId);

        if (!reorderedRes)
        {
          return false;
        }

        observedTrackIds = *reorderedRes;
        return observedTrackIds == expectedTrackIds;
      }));

    CHECK(observedTrackIds == expectedTrackIds);
    REQUIRE(tryPumpGtkEventsUntil(
      [&page]
      {
        auto* const label = findLabelByText(page, "Moved 1 track in Manual Order.");
        return label != nullptr && label->get_visible();
      }));
  }

  TEST_CASE("TrackViewPage - late inline completion preserves the newer published title")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Initial title"}); }};
    auto& runtime = fixture.runtime();
    auto const& library = runtime.library();
    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId);
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto rowCache = TrackRowCache{library, textCatalog};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr}));
    auto columnLayouts = uimodel::TrackColumnLayouts{library.changes()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    // The manual executor can hold the page completion without holding LibraryChanges on GTK's executor.
    auto pageCallbacks = rt::test::ManualExecutor{};
    auto pageRuntime = async::Runtime{pageCallbacks, 1};
    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              textCatalog,
                              pageRuntime,
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader};
    auto window = GtkWindowFixture{};
    window.window().set_default_size(720, 320);
    window.mount(page);
    window.present();

    auto const itemPtr = std::dynamic_pointer_cast<TrackListItem>(modelPtr->get_object(0));
    REQUIRE(itemPtr);
    REQUIRE(itemPtr->trackId() == trackId);
    auto const rowPtr = modelPtr->trackRow(trackId);
    REQUIRE(rowPtr);
    REQUIRE(rowPtr->fieldText(rt::TrackField::Title) == "Initial title");
    auto* const titleLabel = findWidgetByClass<Gtk::Label>(page, "ao-track-title-cell");
    REQUIRE(titleLabel != nullptr);
    REQUIRE(titleLabel->get_mapped());
    REQUIRE(titleLabel->get_text() == "Initial title");
    auto* const titleStack = dynamic_cast<Gtk::Stack*>(titleLabel->get_parent());
    REQUIRE(titleStack != nullptr);
    auto* const entry = dynamic_cast<Gtk::Entry*>(titleStack->get_child_by_name("edit"));
    REQUIRE(entry != nullptr);

    titleStack->set_visible_child("edit");
    REQUIRE(tryEmitFocusEnter(*entry));
    entry->set_text("Inline title");
    emitActivate(*entry);
    REQUIRE(titleStack->get_visible_child() == titleLabel);
    CHECK(titleLabel->get_text() == "Initial title");

    page.setStatusMessage("Awaiting inline completion");
    auto* const statusLabel = findLabelByText(page, "Awaiting inline completion");
    REQUIRE(statusLabel != nullptr);
    auto const checkTitle = [&](char const* phase, char const* expected)
    {
      INFO(phase);
      // Read the held row directly: a cache lookup could refresh it and hide a rollback.
      CHECK(rowPtr->fieldText(rt::TrackField::Title) == expected);
      CHECK(modelPtr->get_object(0) == itemPtr);
      auto* const currentLabel = findWidgetByClass<Gtk::Label>(page, "ao-track-title-cell");
      REQUIRE(currentLabel != nullptr);
      CHECK(currentLabel == titleLabel);
      CHECK(currentLabel->get_mapped());
      CHECK(currentLabel->get_text() == expected);
      auto const optStoredTrack = library.snapshot().trackRow(trackId);
      REQUIRE(optStoredTrack);
      CHECK(optStoredTrack->title == expected);
    };

    REQUIRE(pageCallbacks.tryWaitUntilQueued());
    REQUIRE(pageCallbacks.tryRunOne());
    REQUIRE(tryPumpGtkEventsUntil([&pageCallbacks] { return pageCallbacks.queuedCount() != 0; }));
    REQUIRE(statusLabel->get_visible());
    checkTitle("After the inline edit publishes", "Inline title");

    auto const targetsRes = library.bindTrackTargets(std::array{trackId});
    REQUIRE(targetsRes);
    constexpr auto kLaterTitle = "Later published title";
    auto const laterEditRes = runGtkTask(
      runtime,
      fixture.owner().libraryCommands().updateMetadataAsync(*targetsRes, rt::MetadataPatch{.optTitle = kLaterTitle}));
    REQUIRE(laterEditRes);
    REQUIRE(laterEditRes->status == rt::AuthoringStatus::Applied);
    REQUIRE(pageCallbacks.queuedCount() != 0);
    REQUIRE(statusLabel->get_visible());
    checkTitle("After the newer edit publishes", kLaterTitle);

    // Only the page's Applied handler clears the sentinel; an ignored completion cannot pass.
    REQUIRE(pageCallbacks.tryDrainUntil([statusLabel] { return !statusLabel->get_visible(); }));
    window.drain();
    checkTitle("After the late Applied completion", kLaterTitle);
  }

  TEST_CASE("TrackViewPage - an intervening revision cancels inline metadata without changing the row")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Before"}); }};
    auto& runtime = fixture.runtime();
    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId);
    auto projectionPtr = std::shared_ptr<rt::TrackListProjection>{
      runtime.views().createTransientTrackListProjection(rt::TrackSourceLease{sourcePtr})};
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ResourceImageLoader{runtime.resourceBytes(), imageCache, runtime.async()};
    auto page = TrackViewPage{rt::kAllTracksListId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.async(),
                              fixture.editing(),
                              runtime.views(),
                              thumbnailLoader};
    auto window = Gtk::Window{};
    window.set_child(page);
    window.set_default_size(720, 320);
    window.present();
    drainGtkEvents();

    Gtk::Stack* titleStack = nullptr;

    for (auto* const stack : collectAll<Gtk::Stack>(page))
    {
      auto* const label = dynamic_cast<Gtk::Label*>(stack->get_child_by_name("display"));

      if (label != nullptr && label->get_text() == "Before")
      {
        titleStack = stack;
        break;
      }
    }

    REQUIRE(titleStack != nullptr);
    auto* const entry = dynamic_cast<Gtk::Entry*>(titleStack->get_child_by_name("edit"));
    REQUIRE(entry != nullptr);
    titleStack->set_visible_child("edit");
    REQUIRE(tryEmitFocusEnter(*entry));
    entry->set_text("After");
    REQUIRE(runGtkTask(runtime, fixture.owner().libraryCommands().createListAsync(rt::ListDraft{.name = "Unrelated"})));
    REQUIRE(tryPumpGtkEventsUntil([titleStack] { return titleStack->get_visible_child_name() == "display"; }));

    CHECK(titleStack->get_visible_child_name() == "display");
    auto const rowPtr = rowCache.trackRow(trackId);
    REQUIRE(rowPtr);
    CHECK(rowPtr->fieldText(rt::TrackField::Title) == "Before");
    auto const optTrack = runtime.library().snapshot().trackRow(trackId);
    REQUIRE(optTrack);
    CHECK(optTrack->title == "Before");

    window.close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
