// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("LayoutComponents - the registry describes every shared component the way the vocabulary does",
            "[gtk][unit][layout-component][registry]")
  {
    // GTK is the larger catalog, so it is the one most able to drift: a change
    // made here for a GTK reason must not quietly redefine a type Windows also
    // registers.
    auto fixture = LayoutRuntimeFixture{};
    auto const departures = sharedVocabularyDepartures(fixture.components().catalog());

    for (auto const& departure : departures)
    {
      UNSCOPED_INFO(departure);
    }

    CHECK(departures.empty());
  }

  TEST_CASE("LayoutComponents - the soul button keeps the secondary hold GDK can tell apart",
            "[gtk][unit][layout-component][registry]")
  {
    // The shared floor stops at the three slots Windows can bind, because a
    // catalog that offers a slot its shell refuses fails the document at build
    // time. GDK does raise a distinct secondary hold, so this shell widens the
    // slot back rather than losing a gesture it has always had.
    auto fixture = LayoutRuntimeFixture{};
    auto const optSoul = fixture.components().catalog().descriptor("playback.soulButton");

    REQUIRE(optSoul);
    CHECK(optSoul->actionPolicy.isSlotAllowed(uimodel::LayoutActionSlot::SecondaryLongPress));
  }

  TEST_CASE("LayoutComponents - standard layout registry creates status and semantic components",
            "[gtk][unit][layout-component][registry]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("all registered status and semantic types")
    {
      auto const types = std::to_array<std::string_view>({"status.message",
                                                          "library.listTree",
                                                          "track.table",
                                                          "library.openLibraryButton",
                                                          "app.menuBar",
                                                          "status.playbackDetails",
                                                          "status.nowPlaying",
                                                          "status.importProgress",
                                                          "status.notification",
                                                          "status.trackCount",
                                                          "track.detailScope",
                                                          "track.selectionRegion",
                                                          "track.coverArt",
                                                          "track.fieldGrid",
                                                          "track.detailUndoBar",
                                                          "track.tagEditor",
                                                          "track.quickFilter"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
        std::unique_ptr<LayoutComponent> const compPtr = fixture.create(node);
        CHECK(compPtr != nullptr);
      }
    }
  }
} // namespace ao::gtk::layout::test
