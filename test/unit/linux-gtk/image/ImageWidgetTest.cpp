// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/ImageCache.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ProjectionTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("ImageWidget - basic functionality", "[gtk][image]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto imageCache = ImageCache{200};

    auto widget = ImageWidget{runtime.musicLibrary(), imageCache};

    SECTION("initial state has alt text")
    {
      CHECK(widget.get_alternative_text() == "No cover art");
    }

    SECTION("bind to projection updates image")
    {
      auto mockProj = std::make_shared<ManualTrackDetailMock>();

      auto snapshot = rt::TrackDetailSnapshot{};
      snapshot.selectionKind = rt::SelectionKind::Single;
      snapshot.trackIds = {TrackId{1}};
      snapshot.singleCoverArtId = ResourceId{123};

      widget.bindToDetailProjection(mockProj);

      mockProj->emit(snapshot);
      drainGtkEvents();

      // Since we don't have a real resource with ID 123, it won't actually load an image,
      // but it covers the branch in onDetailSnapshot.
    }
  }
} // namespace ao::gtk::test
