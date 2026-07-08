// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("LayoutComponents - standard layout registry creates status and semantic components",
            "[gtk][unit][layout-component][registry]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("all registered status and semantic types")
    {
      auto const types = std::to_array<std::string_view>({"status.messageLabel",
                                                          "library.listTree",
                                                          "tracks.table",
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
        auto const compPtr = fixture.create(node);
        CHECK(compPtr != nullptr);
      }
    }
  }
} // namespace ao::gtk::layout::test
