// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include <ao/uimodel/layout/component/StatefulLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// YAML serialization — GTK-dependent tests that use makeDefaultLayout
// ---------------------------------------------------------------------------

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("LayoutModel - GTK built-in layout documents define stable preset contracts", "[gtk][unit][layout][model]")
  {
    SECTION("LayoutDocument round-trip via makeDefaultLayout")
    {
      auto const doc = makeDefaultLayout();
      auto tree = ryml::Tree{};
      REQUIRE(LayoutDocumentYamlSchema{}.serialize(tree.rootref(), doc));

      auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
      REQUIRE(decoded);

      CHECK(decoded->version == 1);
      CHECK(decoded->root.type == "box");
      REQUIRE(!decoded->root.children.empty());

      // Verify menu bar is a template
      auto const& menuBar = decoded->root.children[0];
      CHECK(menuBar.type == "template");
      CHECK(menuBar.propertyOr<std::string>("templateId", "") == "app.defaultMenuBar");

      // Verify playback row is a template
      REQUIRE(decoded->root.children.size() > 1);
      auto const& playbackBar = decoded->root.children[1];
      CHECK(playbackBar.id == "playback-bar");
      CHECK(playbackBar.type == "template");
      CHECK(playbackBar.propertyOr<std::string>("templateId", "") == "playback.defaultBar");

      // Verify main paned area is a template (shifted to index 3 due to separator)
      REQUIRE(decoded->root.children.size() > 3);
      auto const& mainPaned = decoded->root.children[3];
      CHECK(mainPaned.id == "main-paned");
      CHECK(mainPaned.type == "template");
      CHECK(mainPaned.propertyOr<std::string>("templateId", "") == "app.defaultMainPaned");

      // Verify status bar region is a template (shifted to index 5 due to separator)
      REQUIRE(decoded->root.children.size() > 5);
      auto const& statusBar = decoded->root.children[5];
      CHECK(statusBar.type == "template");
      CHECK(statusBar.propertyOr<std::string>("templateId", "") == "status.defaultBar");
    }

    SECTION("built-in detail panes start with responsive percentage")
    {
      auto const classicDoc = makeBuiltInLayout(LayoutPresetId::Classic);
      auto const classicDetailSplit = classicDoc.templates.at("app.defaultLayout");

      CHECK(classicDetailSplit.id == "main-workspace-split");
      CHECK(classicDetailSplit.type == "collapsibleSplit");
      CHECK(classicDetailSplit.propertyOr<double>("initialPositionPercent", 0.0) == 0.2);

      auto const modernDoc = makeBuiltInLayout(LayoutPresetId::Modern);
      REQUIRE(!modernDoc.root.children.empty());

      auto const& mainPaned = modernDoc.root.children[0];
      REQUIRE(mainPaned.children.size() == 2);

      auto const& contentShell = mainPaned.children[1];
      REQUIRE(contentShell.children.size() >= 2);

      auto const& modernDetailSplit = contentShell.children[1];
      CHECK(modernDetailSplit.id == "main-workspace-split");
      CHECK(modernDetailSplit.type == "collapsibleSplit");
      CHECK(modernDetailSplit.propertyOr<double>("initialPositionPercent", 0.0) == 0.2);
    }

    SECTION("built-in stateful components have stable ids")
    {
      for (auto const presetId : {LayoutPresetId::Classic, LayoutPresetId::Modern})
      {
        auto const doc = makeBuiltInLayout(presetId);
        auto const prepared = prepareLayout(doc);
        auto missingStatefulIds = std::vector<std::string>{};

        REQUIRE(prepared);

        visitLayoutDocumentNodes(doc,
                                 [&](LayoutNode const& node)
                                 {
                                   if (isStatefulLayoutComponentType(node.type) && node.id.empty())
                                   {
                                     missingStatefulIds.push_back(node.type);
                                   }
                                 });

        CHECK(missingStatefulIds.empty());
        CHECK_FALSE(hasLayoutNodeIdErrors(validateStatefulLayoutNodeIds(*prepared)));
      }
    }

    SECTION("modern bottom bar artwork follows the transport row height")
    {
      auto const doc = makeBuiltInLayout(LayoutPresetId::Modern);
      auto const& modernBar = doc.templates.at("playback.modernBar");

      REQUIRE(modernBar.children.size() >= 2);
      auto const& contentBox = modernBar.children[1];

      REQUIRE(!contentBox.children.empty());
      auto const& startGroup = contentBox.children[0];
      CHECK(startGroup.layoutOr<bool>("vexpand", false) == true);
      CHECK(startGroup.layoutOr<std::string>("valign", "") == "fill");

      REQUIRE(!startGroup.children.empty());
      auto const& image = startGroup.children[0];

      CHECK(image.type == "playback.image");
      CHECK(image.propertyOr<bool>("forceSquare", false) == true);
      CHECK_FALSE(image.layout.contains("widthRequest"));
      CHECK_FALSE(image.layout.contains("heightRequest"));
      CHECK(image.layoutOr<bool>("vexpand", false) == true);
      CHECK(image.layoutOr<std::string>("valign", "") == "fill");
    }

    SECTION("modern header uses allocation breakpoints instead of fixed search width")
    {
      auto const doc = makeBuiltInLayout(LayoutPresetId::Modern);
      REQUIRE(!doc.root.children.empty());

      auto const& mainPaned = doc.root.children[0];
      REQUIRE(mainPaned.children.size() == 2);

      auto const& contentShell = mainPaned.children[1];
      REQUIRE(!contentShell.children.empty());

      auto const& responsiveHeader = contentShell.children[0];
      CHECK(responsiveHeader.type == "responsiveClass");
      CHECK(responsiveHeader.propertyOr<std::int64_t>("compactMax", 0) == 900);
      CHECK(responsiveHeader.propertyOr<std::int64_t>("regularMax", 0) == 1280);
      CHECK(responsiveHeader.layoutOr<bool>("hexpand", false) == true);

      REQUIRE(responsiveHeader.children.size() == 1);
      auto const& header = responsiveHeader.children[0];
      CHECK(header.type == "centerBox");
      REQUIRE(header.children.size() == 3);

      auto const& search = header.children[1];
      CHECK(search.type == "track.quickFilter");
      CHECK_FALSE(search.layout.contains("widthRequest"));
      CHECK(search.layoutOr<bool>("hexpand", false) == true);
    }
  }

  // ---------------------------------------------------------------------------
  // Built-in Presets
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutModel - built-in layout presets preserve preset-specific root classes",
            "[gtk][unit][layout][preset]")
  {
    SECTION("Classic preset is the default and has no modern classes")
    {
      auto const doc = makeDefaultLayout();
      CHECK(doc.root.layoutOr<std::string>("cssClasses", "") != "ao-layout-preset-modern");
    }
  }
} // namespace ao::gtk::layout::test
