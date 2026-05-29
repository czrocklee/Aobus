// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "inspector/TrackInspectorPanel.h"

#include "image/ImageCache.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("TrackInspectorPanel - smoke test", "[gtk][inspector]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto imageCache = ImageCache{200};

    auto window = Gtk::Window{};
    auto panel = TrackInspectorPanel{runtime.musicLibrary(), runtime.mutation(), runtime.sources(), imageCache};
    window.set_child(panel);

    drainGtkEvents();
  }
} // namespace ao::gtk::test
