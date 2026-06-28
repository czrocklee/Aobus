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
    T* sentinelPtr(std::uintptr_t value)
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
      .trackRowCache = sentinelPtr<TrackRowCache>(0x1000),
      .imageCache = sentinelPtr<ImageCache>(0x2000),
      .playbackQueueModel = sentinelPtr<uimodel::playback::PlaybackQueueModel>(0x3000),
      .tagEditController = sentinelPtr<TagEditController>(0x4000),
      .importExportCoordinator = sentinelPtr<portal::ImportExportActions>(0x5000),
      .trackPageHost = sentinelPtr<TrackPageHost>(0x6000),
      .trackPresentationCatalog = sentinelPtr<uimodel::track::TrackPresentationCatalog>(0x7000),
      .trackPresentationPreferences = sentinelPtr<uimodel::track::TrackPresentationPreferenceStore>(0x7100),
      .listNavigationController = sentinelPtr<ListNavigationController>(0x8000),
      .themeController = sentinelPtr<ThemeCoordinator>(0x9000),
    };

    bindServices(track, list, playback, detail, tag, portal, theme, services);

    CHECK(track.pageHost == sentinelPtr<TrackPageHost>(0x6000));
    CHECK(track.presentationCatalog == sentinelPtr<uimodel::track::TrackPresentationCatalog>(0x7000));
    CHECK(track.presentationPreferences == sentinelPtr<uimodel::track::TrackPresentationPreferenceStore>(0x7100));
    CHECK(track.trackRowCache == sentinelPtr<TrackRowCache>(0x1000));
    CHECK(list.navigationController == sentinelPtr<ListNavigationController>(0x8000));
    CHECK(playback.queueModel == sentinelPtr<uimodel::playback::PlaybackQueueModel>(0x3000));
    CHECK(detail.imageCache == sentinelPtr<ImageCache>(0x2000));
    CHECK(tag.editController == sentinelPtr<TagEditController>(0x4000));
    CHECK(portal.coordinator == sentinelPtr<portal::ImportExportActions>(0x5000));
    CHECK(theme.themeController == sentinelPtr<ThemeCoordinator>(0x9000));
  }
} // namespace ao::gtk::layout::test
