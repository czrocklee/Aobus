// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/LayoutContext.h"

#include "app/GtkUiServices.h"
#include "portal/ImportExportActions.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::gtk::layout::test
{
  namespace
  {
    template<typename T>
    T* sentinelPointer(std::uintptr_t value)
    {
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      return reinterpret_cast<T*>(value);
    }
  } // namespace

  TEST_CASE("bindServices copies each service into its subsystem context", "[gtk][unit][layout][runtime]")
  {
    auto track = TrackUiContext{};
    auto list = ListUiContext{};
    auto playback = PlaybackUiContext{};
    auto detail = DetailUiContext{};
    auto tag = TagUiContext{};
    auto portal = PortalContext{};
    auto theme = ThemeUiContext{};

    auto const services = GtkUiServices{
      .trackRowCache = sentinelPointer<TrackRowCache>(0x1000),
      .imageCache = sentinelPointer<ImageCache>(0x2000),
      .playbackQueueModel = sentinelPointer<uimodel::PlaybackQueueModel>(0x3000),
      .playbackCommandSurface = sentinelPointer<uimodel::PlaybackCommandSurface>(0x3100),
      .tagEditController = sentinelPointer<TagEditController>(0x4000),
      .importExportCoordinator = sentinelPointer<portal::ImportExportActions>(0x5000),
      .trackPageHost = sentinelPointer<TrackPageHost>(0x6000),
      .trackPresentationCatalog = sentinelPointer<uimodel::TrackPresentationCatalog>(0x7000),
      .trackPresentationPreferences = sentinelPointer<uimodel::ListPresentationPreferenceStore>(0x7100),
      .listNavigationController = sentinelPointer<ListNavigationController>(0x8000),
      .themeController = sentinelPointer<ThemeCoordinator>(0x9000),
    };

    bindServices(track, list, playback, detail, tag, portal, theme, services);

    CHECK(track.pageHost == sentinelPointer<TrackPageHost>(0x6000));
    CHECK(track.presentationCatalog == sentinelPointer<uimodel::TrackPresentationCatalog>(0x7000));
    CHECK(track.presentationPreferences == sentinelPointer<uimodel::ListPresentationPreferenceStore>(0x7100));
    CHECK(track.trackRowCache == sentinelPointer<TrackRowCache>(0x1000));
    CHECK(list.navigationController == sentinelPointer<ListNavigationController>(0x8000));
    CHECK(playback.queueModel == sentinelPointer<uimodel::PlaybackQueueModel>(0x3000));
    CHECK(playback.commandSurface == sentinelPointer<uimodel::PlaybackCommandSurface>(0x3100));
    CHECK(detail.imageCache == sentinelPointer<ImageCache>(0x2000));
    CHECK(tag.editController == sentinelPointer<TagEditController>(0x4000));
    CHECK(portal.coordinator == sentinelPointer<portal::ImportExportActions>(0x5000));
    CHECK(theme.themeController == sentinelPointer<ThemeCoordinator>(0x9000));
  }
} // namespace ao::gtk::layout::test
