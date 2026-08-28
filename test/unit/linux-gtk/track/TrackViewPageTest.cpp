// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "../../TestFixtureSupport.h"
#include "image/CoverArtView.h"
#include "image/ImageCache.h"
#include "image/ResourceImageLoader.h"
#include "layout/LayoutConstants.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowBinding.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdk.h>
#include <glib-object.h>
#include <glibmm/value.h>
#include <gtkmm/box.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
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

  TEST_CASE("TrackViewPage - initializes localized list controls and geometry", "[gtk][unit][geometry][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto albumTrackId = kInvalidTrackId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     { albumTrackId = addAlbumTrack(musicLibrary, "Album"); }};
    auto& runtime = fixture.runtime();
    auto const textCatalog = ao::test::messageCatalog("de-DE");
    auto cache = TrackRowCache{runtime.library(), textCatalog};
    auto imageCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto thumbnailLoader = ResourceImageLoader{byteLoader, imageCache, runtime.async()};
    auto window = Gtk::Window{};

    auto modelPtr = TrackListModel::create(cache);
    auto columnLayouts = uimodel::TrackColumnLayouts{};

    auto page = TrackViewPage{rt::kAllTracksListId, modelPtr, columnLayouts, textCatalog, runtime, thumbnailLoader};
    window.set_child(page);

    SECTION("initial state")
    {
      CHECK(page.listId() == rt::kAllTracksListId);
      CHECK(page.projection() == nullptr);
    }

    SECTION("status message shows then hides the status label")
    {
      page.setStatusMessage("Loading...");
      auto* const label = findLabelByText(page, "Loading...");
      REQUIRE(label != nullptr);
      CHECK(label->get_visible());

      page.clearStatusMessage();
      CHECK_FALSE(label->get_visible());
    }

    SECTION("album grouped section header reserves a fixed cover slot")
    {
      auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
      sourcePtr->addInitial(albumTrackId);

      auto projectionPtr = std::make_shared<rt::TrackListProjection>(
        rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
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
      CHECK(coverArt->showingPlaceholder());
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

      auto projectionPtr = std::make_shared<rt::TrackListProjection>(
        rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
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
      CHECK(coverArt->showingPlaceholder());
      CHECK(coverArt->placeholderPresentation().monogram == "23");
    }
  }

  TEST_CASE("TrackViewPage - large projections materialize only the GTK prefetch window",
            "[gtk][regression][track-view]")
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
    auto projectionPtr = std::make_shared<rt::TrackListProjection>(
      rt::kInvalidViewId, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);
    auto imageCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto thumbnailLoader = ResourceImageLoader{byteLoader, imageCache, runtime.async()};

    auto materializedRowsForPage = [&]
    {
      auto columnLayouts = uimodel::TrackColumnLayouts{};
      auto page = TrackViewPage{
        rt::kAllTracksListId, modelPtr, columnLayouts, ao::test::englishMessageCatalog(), runtime, thumbnailLoader};
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

    rowCache.clearCache();
    projectionPtr->setPresentation(rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album}},
    });

    auto const groupedRows = materializedRowsForPage();
    CHECK(groupedRows > 0);
    CHECK(groupedRows < kMaximumPrefetchedRows);
  }

  TEST_CASE("TrackViewPage - drag handle follows the shared List order capability", "[gtk][unit][track][list-order]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{[](library::MusicLibrary& musicLibrary)
                                     { std::ignore = addAlbumTrack(musicLibrary, "Album"); }};
    auto& runtime = fixture.runtime();
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createList(rt::ListDraft{.name = "Ordered"})));
    auto const* manual = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
    REQUIRE(manual != nullptr);
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate(rt::NavigationRequest{
      .target = rt::FilteredListTarget{.listId = listId, .filterExpression = ""},
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
    auto columnLayouts = uimodel::TrackColumnLayouts{};
    auto imageCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto thumbnailLoader = ResourceImageLoader{byteLoader, imageCache, runtime.async()};
    auto page = TrackViewPage{listId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime,
                              thumbnailLoader,
                              manual->spec,
                              viewId};
    auto windowFixture = GtkWindowFixture{};
    windowFixture.mount(page);
    windowFixture.present();

    CHECK(page.hasOrderDragHandle());
    auto* const dragHandle = findWidgetByClass<Gtk::Box>(page, "ao-order-drag-handle");
    REQUIRE(dragHandle != nullptr);
    CHECK(dragHandle->get_tooltip_text() == "Drag to rearrange tracks in Manual Order");
    CHECK(hasAccessibleLabel(*dragHandle, "Rearrange track"));

    REQUIRE(runtime.views().setPresentation(viewId, rt::defaultTrackPresentationSpec()));
    page.applyPresentation(rt::defaultTrackPresentationSpec());
    CHECK_FALSE(page.hasOrderDragHandle());

    REQUIRE(runtime.views().setPresentation(viewId, manual->spec));
    page.applyPresentation(manual->spec);
    CHECK(page.hasOrderDragHandle());

    REQUIRE(runtime.views().setFilter(viewId, "true"));
    page.applyPresentation(manual->spec);
    CHECK_FALSE(page.hasOrderDragHandle());

    REQUIRE(runtime.views().setFilter(viewId, ""));
    page.refreshOrderCapabilities();
    CHECK(page.hasOrderDragHandle());

    REQUIRE(runtime.views().setFilter(viewId, "("));
    page.refreshOrderCapabilities();
    CHECK_FALSE(page.hasOrderDragHandle());
    CHECK(page.orderCapabilities().disabledReason ==
          "Fix the List or quick-filter expression before changing its order.");
  }

  TEST_CASE("TrackViewPage - dropping a drag handle preserves the order submission", "[gtk][regression][list-order]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{[](library::MusicLibrary& musicLibrary)
                                     {
                                       std::ignore = addAlbumTrack(musicLibrary, "First");
                                       std::ignore = addAlbumTrack(musicLibrary, "Second");
                                     }};
    auto& runtime = fixture.runtime();
    auto const listId = ao::test::requireValue(
      runGtkTask(runtime, runtime.library().commands().createList(rt::ListDraft{.name = "Ordered"})));
    auto const* manual = rt::builtinTrackPresentationPreset(rt::kListOrderTrackPresentationId);
    REQUIRE(manual != nullptr);
    auto const viewId = ao::test::requireValue(runtime.workspace().navigate(rt::NavigationRequest{
      .target = rt::FilteredListTarget{.listId = listId, .filterExpression = ""},
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = manual->spec,
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
    auto columnLayouts = uimodel::TrackColumnLayouts{};
    auto imageCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto thumbnailLoader = ResourceImageLoader{byteLoader, imageCache, runtime.async()};
    auto page = TrackViewPage{listId,
                              modelPtr,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime,
                              thumbnailLoader,
                              manual->spec,
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
    REQUIRE(pumpGtkEventsUntil(
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
  }

  TEST_CASE("TrackViewPage - an intervening revision cancels inline metadata without changing the row",
            "[gtk][regression][track-view][metadata]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto trackId = kInvalidTrackId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        { trackId = library::test::addTrackWithUniqueFixtureUri(musicLibrary, {.title = "Before"}); }};
    auto& runtime = fixture.runtime();
    auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
    sourcePtr->addInitial(trackId);
    auto projectionPtr = std::make_shared<rt::TrackListProjection>(
      rt::kInvalidViewId, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
    auto rowCache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);
    auto columnLayouts = uimodel::TrackColumnLayouts{};
    auto imageCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto thumbnailLoader = ResourceImageLoader{byteLoader, imageCache, runtime.async()};
    auto page = TrackViewPage{
      rt::kAllTracksListId, modelPtr, columnLayouts, ao::test::englishMessageCatalog(), runtime, thumbnailLoader};
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
    REQUIRE(emitFocusEnter(*entry));
    entry->set_text("After");
    REQUIRE(runGtkTask(runtime, runtime.library().commands().createList(rt::ListDraft{.name = "Unrelated"})));
    REQUIRE(pumpGtkEventsUntil([titleStack] { return titleStack->get_visible_child_name() == "display"; }));

    CHECK(titleStack->get_visible_child_name() == "display");
    auto const rowPtr = rowCache.trackRow(trackId);
    REQUIRE(rowPtr);
    CHECK(rowPtr->fieldText(rt::TrackField::Title) == "Before");
    auto const transaction = runtime.musicLibrary().readTransaction();
    auto const optView =
      runtime.musicLibrary().tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Hot);
    REQUIRE(optView);
    CHECK(optView->metadata().title() == "Before");

    window.close();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
