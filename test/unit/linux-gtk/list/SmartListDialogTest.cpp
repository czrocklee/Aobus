// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/SmartListDialog.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("SmartListDialog - initial draft", "[gtk][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{fixture.runtime().library()};

    auto dialog = SmartListDialog{window, fixture.runtime(), rt::kAllTracksListId, cache};
    window.set_child(dialog);

    // Rebuild happens in idle task
    drainGtkEvents();
    drainGtkEvents();

    auto const draft = dialog.draft();
    CHECK(dialog.editListId() == kInvalidListId);
    CHECK(draft.parentId == rt::kAllTracksListId);
    CHECK(draft.kind == rt::LibraryWriter::ListKind::Smart);
  }
} // namespace ao::gtk::test
