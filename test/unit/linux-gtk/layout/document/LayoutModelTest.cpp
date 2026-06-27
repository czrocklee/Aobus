// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutNodeId.h>
#include <ao/uimodel/layout/LayoutYaml.h>
#include <ao/uimodel/layout/StatefulLayoutComponentType.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// YAML serialization — GTK-dependent tests that use createDefaultLayout
// ---------------------------------------------------------------------------

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  namespace yaml = ao::yaml;

  TEST_CASE("GTK built-in layout documents define stable preset contracts", "[gtk][unit][layout][model]")
  {
    SECTION("LayoutDocument round-trip via createDefaultLayout")
    {
      auto const doc = createDefaultLayout();
      auto tree = ryml::Tree{};
      yaml::write(tree.rootref(), doc);

      auto decoded = LayoutDocument{};
      REQUIRE(yaml::read(tree.rootref(), decoded));

      CHECK(decoded.version == 1);
      CHECK(decoded.root.type == "box");
      REQUIRE(!decoded.root.children.empty());

      // Verify menu bar is a template
      auto const& menuBar = decoded.root.children[0];
      CHECK(menuBar.type == "template");
      CHECK(menuBar.getProp<std::string>("templateId", "") == "app.defaultMenuBar");

      // Verify playback row is a template
      REQUIRE(decoded.root.children.size() > 1);
      auto const& playbackBar = decoded.root.children[1];
      CHECK(playbackBar.id == "playback-bar");
      CHECK(playbackBar.type == "template");
      CHECK(playbackBar.getProp<std::string>("templateId", "") == "playback.defaultBar");

      // Verify main paned area is a template (shifted to index 3 due to separator)
      REQUIRE(decoded.root.children.size() > 3);
      auto const& mainPaned = decoded.root.children[3];
      CHECK(mainPaned.id == "main-paned");
      CHECK(mainPaned.type == "template");
      CHECK(mainPaned.getProp<std::string>("templateId", "") == "app.defaultMainPaned");

      // Verify status bar region is a template (shifted to index 5 due to separator)
      REQUIRE(decoded.root.children.size() > 5);
      auto const& statusBar = decoded.root.children[5];
      CHECK(statusBar.type == "template");
      CHECK(statusBar.getProp<std::string>("templateId", "") == "status.defaultBar");
    }

    SECTION("built-in detail panes start with responsive percentage")
    {
      auto const classicDoc = createBuiltInLayout(LayoutPresetId::Classic);
      auto const classicDetailSplit = classicDoc.templates.at("app.defaultLayout");

      CHECK(classicDetailSplit.id == "main-workspace-split");
      CHECK(classicDetailSplit.type == "collapsibleSplit");
      CHECK(classicDetailSplit.getProp<double>("initialPositionPercent", 0.0) == 0.2);

      auto const modernDoc = createBuiltInLayout(LayoutPresetId::Modern);
      REQUIRE(!modernDoc.root.children.empty());

      auto const& mainPaned = modernDoc.root.children[0];
      REQUIRE(mainPaned.children.size() == 2);

      auto const& contentShell = mainPaned.children[1];
      REQUIRE(contentShell.children.size() >= 2);

      auto const& modernDetailSplit = contentShell.children[1];
      CHECK(modernDetailSplit.id == "main-workspace-split");
      CHECK(modernDetailSplit.type == "collapsibleSplit");
      CHECK(modernDetailSplit.getProp<double>("initialPositionPercent", 0.0) == 0.2);
    }

    SECTION("built-in stateful components have stable ids")
    {
      for (auto const presetId : {LayoutPresetId::Classic, LayoutPresetId::Modern})
      {
        auto const doc = createBuiltInLayout(presetId);
        auto missingStatefulIds = std::vector<std::string>{};

        visitLayoutDocumentNodes(doc,
                                 [&](LayoutNode const& node)
                                 {
                                   if (isStatefulLayoutComponentType(node.type) && node.id.empty())
                                   {
                                     missingStatefulIds.push_back(node.type);
                                   }
                                 });

        CHECK(missingStatefulIds.empty());
        CHECK_FALSE(hasLayoutNodeIdErrors(validateStatefulLayoutNodeIds(doc)));
      }
    }

    SECTION("modern bottom bar artwork follows the transport row height")
    {
      auto const doc = createBuiltInLayout(LayoutPresetId::Modern);
      auto const& modernBar = doc.templates.at("playback.modernBar");

      REQUIRE(modernBar.children.size() >= 2);
      auto const& contentBox = modernBar.children[1];

      REQUIRE(!contentBox.children.empty());
      auto const& startGroup = contentBox.children[0];
      CHECK(startGroup.getLayout<bool>("vexpand", false) == true);
      CHECK(startGroup.getLayout<std::string>("valign", "") == "fill");

      REQUIRE(!startGroup.children.empty());
      auto const& image = startGroup.children[0];

      CHECK(image.type == "playback.image");
      CHECK(image.getProp<bool>("forceSquare", false) == true);
      CHECK_FALSE(image.layout.contains("widthRequest"));
      CHECK_FALSE(image.layout.contains("heightRequest"));
      CHECK(image.getLayout<bool>("vexpand", false) == true);
      CHECK(image.getLayout<std::string>("valign", "") == "fill");
    }

    SECTION("modern header uses allocation breakpoints instead of fixed search width")
    {
      auto const doc = createBuiltInLayout(LayoutPresetId::Modern);
      REQUIRE(!doc.root.children.empty());

      auto const& mainPaned = doc.root.children[0];
      REQUIRE(mainPaned.children.size() == 2);

      auto const& contentShell = mainPaned.children[1];
      REQUIRE(!contentShell.children.empty());

      auto const& responsiveHeader = contentShell.children[0];
      CHECK(responsiveHeader.type == "responsiveClass");
      CHECK(responsiveHeader.getProp<std::int64_t>("compactMax", 0) == 900);
      CHECK(responsiveHeader.getProp<std::int64_t>("regularMax", 0) == 1280);
      CHECK(responsiveHeader.getLayout<bool>("hexpand", false) == true);

      REQUIRE(responsiveHeader.children.size() == 1);
      auto const& header = responsiveHeader.children[0];
      CHECK(header.type == "centerBox");
      REQUIRE(header.children.size() == 3);

      auto const& search = header.children[1];
      CHECK(search.type == "track.quickFilter");
      CHECK_FALSE(search.layout.contains("widthRequest"));
      CHECK(search.getLayout<bool>("hexpand", false) == true);
    }
  }

  // ---------------------------------------------------------------------------
  // Built-in Presets
  // ---------------------------------------------------------------------------
  TEST_CASE("Built-in layout presets preserve preset-specific root classes", "[gtk][unit][layout][presets]")
  {
    SECTION("Classic preset is the default and has no modern classes")
    {
      auto const doc = createDefaultLayout();
      CHECK(doc.root.getLayout<std::string>("cssClasses", "") != "ao-layout-preset-modern");
    }
  }
} // namespace ao::gtk::layout::test
