// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include <ao/uimodel/layout/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// YAML serialization — GTK-dependent tests that use createDefaultLayout
// ---------------------------------------------------------------------------

namespace ao::gtk::layout::test
{
  TEST_CASE("Layout model GTK serialization", "[layout][unit][gtk][model]")
  {
    SECTION("LayoutDocument round-trip via createDefaultLayout")
    {
      auto const doc = createDefaultLayout();
      auto tree = ryml::Tree{};
      rt::yaml::write(tree.rootref(), doc);

      auto decoded = LayoutDocument{};
      REQUIRE(rt::yaml::read(tree.rootref(), decoded));

      CHECK(decoded.version == 1);
      CHECK(decoded.root.type == "box");
      REQUIRE(!decoded.root.children.empty());
      CHECK(decoded.root.children[0].type == "app.menuBar");

      // Verify playback row is a template
      REQUIRE(decoded.root.children.size() > 1);
      auto const& playbackRow = decoded.root.children[1];
      CHECK(playbackRow.id == "playback-row");
      CHECK(playbackRow.type == "template");
      CHECK(playbackRow.getProp<std::string>("templateId", "") == "playback.defaultBar");

      // Verify status bar is a template inside a box
      REQUIRE(decoded.root.children.size() > 3);
      auto const& statusRegion = decoded.root.children[3];
      CHECK(statusRegion.type == "box");
      CHECK(statusRegion.getLayout<std::string>("cssClasses", "") == "ao-status-region");
      REQUIRE(!statusRegion.children.empty());
      auto const& statusTemplate = statusRegion.children[0];
      CHECK(statusTemplate.type == "template");
      CHECK(statusTemplate.getProp<std::string>("templateId", "") == "status.defaultBar");
    }
  }
} // namespace ao::gtk::layout::test
