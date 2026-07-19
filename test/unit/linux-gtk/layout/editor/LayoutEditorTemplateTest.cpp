// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;

  TEST_CASE("LayoutEditorTemplate - built-in layout templates expose expected component structure",
            "[gtk][unit][layout][editor]")
  {
    SECTION("builtInTemplates returns all 8 built-ins")
    {
      auto const templates = builtInTemplates();

      CHECK(templates.contains("playback.compactControls"));
      CHECK(templates.contains("playback.transportGroup"));
      CHECK(templates.contains("playback.defaultBar"));
      CHECK(templates.contains("library.defaultListPane"));
      CHECK(templates.contains("track.defaultDetailPane"));
      CHECK(templates.contains("status.defaultBar"));
      CHECK(templates.contains("tracks.defaultWorkspace"));
      CHECK(templates.contains("app.defaultLayout"));
      CHECK(templates.contains("track.selectionDetailPane"));

      int const expectedCount = 9;
      CHECK(templates.size() >= expectedCount);
    }

    SECTION("playback.transportGroup has 2 children and linked class")
    {
      auto const templates = builtInTemplates();
      auto const& group = templates.at("playback.transportGroup");

      CHECK(group.type == "box");
      CHECK(group.propertyOr<std::int64_t>("spacing", -1) == 0);

      auto const classes = group.layoutOr<std::vector<std::string>>("cssClasses", {});
      CHECK(std::ranges::contains(classes, std::string_view{"linked"}));
      CHECK(group.children.size() == 2);
    }

    SECTION("playback.defaultBar contains all expected children")
    {
      auto const templates = builtInTemplates();
      auto const& bar = templates.at("playback.defaultBar");

      CHECK(bar.type == "box");

      // 3 children: left fixed controls, flexible seek slider, right fixed status controls.
      int const expectedChildren = 3;
      CHECK(bar.children.size() == expectedChildren);
      CHECK(bar.children[0].type == "box");
      CHECK(bar.children[0].children.size() == 2);
      CHECK(bar.children[0].children[0].type == "playback.soulButton");
      CHECK(bar.children[0].children[1].type == "template");
      CHECK(bar.children[0].children[1].propertyOr<std::string>("templateId", "") == "playback.transportGroup");
      CHECK(bar.children[1].type == "playback.seekSlider");
      CHECK(bar.children[1].layoutOr<bool>("hexpand", false));
      CHECK(bar.children[2].type == "box");
      CHECK(bar.children[2].children.size() == 2);
      CHECK(bar.children[2].children[0].type == "playback.timeLabel");
      CHECK(bar.children[2].children[1].type == "playback.volumeControl");

      // Grouping regions carry ao-grouping-region CSS class.
      CHECK(bar.children[0].layoutOr<std::string>("cssClasses", "") == "ao-grouping-region");
      CHECK(bar.children[2].layoutOr<std::string>("cssClasses", "") == "ao-grouping-region");
    }

    SECTION("status.defaultBar template preserves right-side status order")
    {
      auto const templates = builtInTemplates();
      auto const& bar = templates.at("status.defaultBar");

      CHECK(bar.type == "box");

      // 8 children: playbackDetails, spacer, nowPlaying, spacer, activityStatus,
      // selectionInfo, separator, trackCount.
      int const expectedChildren = 8;
      CHECK(bar.children.size() == expectedChildren);
      REQUIRE(bar.children.size() >= 8);
      CHECK(bar.children[4].type == "status.activityStatus");
      CHECK(bar.children[5].type == "status.selectionInfo");
      CHECK(bar.children[6].type == "separator");
      CHECK(bar.children[7].type == "status.trackCount");
      CHECK(bar.children[4].propertyOr<std::string>("variant", "") == "classicInline");
      CHECK(bar.children[4].propertyOr<std::string>("idleBehavior", "") == "hidden");
    }

    SECTION("modern layout keeps selection counts in header and activity in bottom bar")
    {
      auto const doc = makeBuiltInLayout(LayoutPresetId::Modern);
      REQUIRE(doc.root.children.size() >= 2);

      auto const& mainPaned = doc.root.children[0];
      REQUIRE(mainPaned.children.size() == 2);
      auto const& contentShell = mainPaned.children[1];
      REQUIRE(!contentShell.children.empty());
      auto const& responsiveHeader = contentShell.children[0];
      REQUIRE(!responsiveHeader.children.empty());
      auto const& header = responsiveHeader.children[0];
      REQUIRE(header.children.size() >= 3);
      auto const& headerStats = header.children[2];

      REQUIRE(headerStats.children.size() == 2);
      CHECK(headerStats.children[0].type == "status.selectionInfo");
      CHECK(headerStats.children[1].type == "status.trackCount");

      auto const& modernBar = doc.templates.at("playback.modernBar");
      REQUIRE(modernBar.children.size() >= 2);
      auto const& bottomContent = modernBar.children[1];
      REQUIRE(bottomContent.children.size() >= 3);
      auto const& bottomEnd = bottomContent.children[2];

      REQUIRE(!bottomEnd.children.empty());
      CHECK(bottomEnd.children[0].type == "status.activityStatus");
      CHECK(bottomEnd.children[0].propertyOr<std::string>("variant", "") == "ambient");
      CHECK(bottomEnd.children[0].propertyOr<std::string>("idleBehavior", "") == "hidden");
    }

    SECTION("track.selectionDetailPane keeps field grid scroll at layout level")
    {
      auto const templates = builtInTemplates();
      auto const& pane = templates.at("track.selectionDetailPane");

      LayoutNode const* detailContent = nullptr;

      auto visit = std::function<void(LayoutNode const&)>{};
      visit = [&](LayoutNode const& node)
      {
        if (std::ranges::any_of(node.children, [](LayoutNode const& child) { return child.type == "track.coverArt"; }))
        {
          detailContent = &node;
        }

        for (auto const& child : node.children)
        {
          visit(child);
        }
      };

      visit(pane);

      REQUIRE(detailContent != nullptr);
      REQUIRE(detailContent->children.size() == 4);
      CHECK(detailContent->children[0].type == "track.coverArt");
      CHECK(detailContent->children[1].type == "scroll");
      REQUIRE(detailContent->children[1].children.size() == 1);
      CHECK(detailContent->children[1].children[0].type == "track.fieldGrid");
      CHECK(detailContent->children[2].type == "track.detailUndoBar");
      CHECK(detailContent->children[3].type == "track.tagEditor");
    }

    SECTION("template expansion via expandNode in build")
    {
      auto fixture = ao::gtk::layout::test::LayoutRuntimeFixture{"io.github.aobus.template_test"};

      auto doc = LayoutDocument{};
      doc.version = 1;
      doc.templates = builtInTemplates();
      doc.root.type = "template";
      doc.root.props["templateId"] = LayoutValue{std::string{"playback.compactControls"}};

      auto const compPtr = fixture.layoutRuntime().build(fixture.context(), doc);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      CHECK(box != nullptr);
    }

    SECTION("recursive template reference produces error")
    {
      auto fixture = ao::gtk::layout::test::LayoutRuntimeFixture{"io.github.aobus.recursive_test"};

      auto doc = LayoutDocument{};
      doc.version = 1;
      doc.templates["selfRef"] = LayoutNode{.type = "template"};
      doc.templates["selfRef"].props["templateId"] = LayoutValue{std::string{"selfRef"}};
      doc.root.type = "template";
      doc.root.props["templateId"] = LayoutValue{std::string{"selfRef"}};

      auto const compPtr = fixture.layoutRuntime().build(fixture.context(), doc);

      REQUIRE(compPtr != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(&compPtr->widget()) != nullptr);
    }

    SECTION("template YAML round-trip")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.templates = builtInTemplates();

      auto tree = ryml::Tree{};
      REQUIRE(LayoutDocumentYamlSchema{}.serialize(tree.rootref(), doc));

      auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
      REQUIRE(decoded);

      REQUIRE(decoded->templates.contains("playback.compactControls"));
      CHECK(decoded->templates.at("playback.compactControls").type == "box");
    }
  }
} // namespace ao::gtk::layout::editor::test
