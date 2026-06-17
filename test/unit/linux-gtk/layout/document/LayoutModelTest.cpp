// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutNode.h"
#include "layout/state/LayoutNodeId.h"
#include "layout/state/StatefulLayoutComponentType.h"
#include <ao/uimodel/layout/LayoutYaml.h>
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// YAML serialization — GTK-dependent tests that use createDefaultLayout
// ---------------------------------------------------------------------------

namespace ao::gtk::layout::test
{
  namespace yaml = ao::yaml;

  TEST_CASE("Layout model GTK serialization", "[layout][unit][gtk][model]")
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

    SECTION("duplicate stateful ids are errors after template expansion")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {
        LayoutNode{.id = "shared-split", .type = "split"},
        LayoutNode{.id = "shared-split", .type = "collapsibleSplit"},
      };

      auto const diagnostics = validateStatefulLayoutNodeIds(doc);

      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].severity == LayoutNodeIdDiagnosticSeverity::Error);
      CHECK(diagnostics[0].componentId == "shared-split");
      CHECK(hasLayoutNodeIdErrors(diagnostics));
    }

    SECTION("anonymous stateful nodes warn but remain valid")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";

      auto const diagnostics = validateStatefulLayoutNodeIds(doc);

      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].severity == LayoutNodeIdDiagnosticSeverity::Warning);
      CHECK(diagnostics[0].componentType == "split");
      CHECK_FALSE(hasLayoutNodeIdErrors(diagnostics));
    }

    SECTION("non-stateful duplicate ids do not create ambiguous runtime state keys")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {
        LayoutNode{.id = "same", .type = "spacer"},
        LayoutNode{.id = "same", .type = "separator"},
      };

      CHECK(validateStatefulLayoutNodeIds(doc).empty());
    }

    SECTION("template expansion participates in duplicate detection")
    {
      auto doc = LayoutDocument{};
      doc.templates["pane"] = LayoutNode{.id = "templated-split", .type = "split"};
      doc.root.type = "box";
      doc.root.children = {
        LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"pane"}}}}},
        LayoutNode{.type = "template", .props = {{"templateId", LayoutValue{std::string{"pane"}}}}},
      };

      auto const diagnostics = validateStatefulLayoutNodeIds(doc);

      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].componentId == "templated-split");
      CHECK(diagnostics[0].severity == LayoutNodeIdDiagnosticSeverity::Error);
    }

    SECTION("duplicate stateful ids are errors even when the type matches")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {
        LayoutNode{.id = "shared", .type = "split"},
        LayoutNode{.id = "shared", .type = "split"},
      };

      auto const diagnostics = validateStatefulLayoutNodeIds(doc);
      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].severity == LayoutNodeIdDiagnosticSeverity::Error);
      CHECK(diagnostics[0].componentId == "shared");
      CHECK(hasLayoutNodeIdErrors(diagnostics));
    }

    SECTION("anonymous non-stateful duplicates do not warn")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {
        LayoutNode{.type = "spacer"},
        LayoutNode{.type = "spacer"},
      };

      CHECK(validateStatefulLayoutNodeIds(doc).empty());
    }

    SECTION("new stateful ids are stable and unique across root and templates")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {LayoutNode{.id = "split-new", .type = "split"}};
      doc.templates["copy"] = LayoutNode{.id = "split-new-2", .type = "split"};

      CHECK(makeUniqueLayoutNodeId(doc, "split", "new") == "split-new-3");
    }

    SECTION("freshening a copied stateful subtree replaces ids recursively")
    {
      auto owner = LayoutDocument{};
      owner.root.type = "box";
      owner.root.children = {LayoutNode{.id = "split-original", .type = "split"}};

      auto copy = LayoutNode{.id = "split-original", .type = "split"};
      copy.children = {LayoutNode{.id = "child", .type = "spacer"}, LayoutNode{.type = "collapsibleSplit"}};

      freshenLayoutNodeIds(copy, owner);

      CHECK(copy.id != "split-original");
      CHECK(copy.id == "split-split-original");
      REQUIRE(copy.children.size() == 2);
      CHECK(copy.children[0].id == "spacer-child");
      CHECK(copy.children[1].id == "collapsiblesplit-copy");
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

    SECTION("LayoutDocument round-trip preserves layout props and child order")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.id = "root";

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.layout["hexpand"] = LayoutValue{true};
      c1.layout["vexpand"] = LayoutValue{true};
      doc.root.children.push_back(c1);

      auto c2 = LayoutNode{};
      c2.type = "scroll";
      c2.id = "scroller";
      c2.layout["vexpand"] = LayoutValue{true};
      c2.props["hscrollPolicy"] = LayoutValue{std::string{"never"}};
      doc.root.children.push_back(c2);

      auto tree = ryml::Tree{};
      yaml::write(tree.rootref(), doc);

      auto decoded = LayoutDocument{};
      REQUIRE(yaml::read(tree.rootref(), decoded));

      REQUIRE(decoded.root.children.size() == 2);
      CHECK(decoded.root.children[0].type == "spacer");
      CHECK(decoded.root.children[0].layout.at("hexpand").asBool() == true);
      CHECK(decoded.root.children[0].layout.at("vexpand").asBool() == true);
      CHECK(decoded.root.children[1].type == "scroll");
      CHECK(decoded.root.children[1].id == "scroller");
      CHECK(decoded.root.children[1].layout.at("vexpand").asBool() == true);
      CHECK(decoded.root.children[1].props.at("hscrollPolicy").asString() == "never");
    }

    SECTION("YAML decode tolerates missing optional fields")
    {
      auto const* yaml = R"(
      version: 1
      root:
        type: box
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);

      auto decoded = LayoutDocument{};
      REQUIRE(yaml::read(tree.rootref(), decoded));
      CHECK(decoded.version == 1);
      CHECK(decoded.root.type == "box");
      CHECK(decoded.root.id.empty());
      CHECK(decoded.root.children.empty());
      CHECK(decoded.root.props.empty());
    }

    SECTION("YAML decode tolerates fields set to empty string")
    {
      auto const* yaml = R"(
      version: 1
      root:
        id: ""
        type: spacer
    )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(yaml), &tree);

      auto decoded = LayoutDocument{};
      REQUIRE(yaml::read(tree.rootref(), decoded));
      CHECK(decoded.root.type == "spacer");
      CHECK(decoded.root.id.empty());
    }

    SECTION("LayoutValue serializes and decodes double")
    {
      auto const v = LayoutValue{3.14};
      auto tree = ryml::Tree{};
      yaml::write(tree.rootref(), v);

      auto decoded = LayoutValue{};
      REQUIRE(yaml::read(tree.rootref(), decoded));
      CHECK(decoded.asDouble() == 3.14);
    }
  }

  // ---------------------------------------------------------------------------
  // LayoutValue cross-type coercion
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutValue coercion", "[layout][model]")
  {
    SECTION("asString coerces bool")
    {
      auto v = LayoutValue{true};
      CHECK(v.asString() == "true");

      auto vf = LayoutValue{false};
      CHECK(vf.asString() == "false");
    }

    SECTION("asString coerces int")
    {
      auto v = LayoutValue{static_cast<std::int64_t>(42)};
      CHECK(v.asString() == "42");
    }

    SECTION("asString coerces double")
    {
      auto const v = LayoutValue{3.14};
      CHECK(v.asString() == "3.14");
    }

    SECTION("asInt coerces string")
    {
      auto const v = LayoutValue{std::string{"99"}};
      CHECK(v.asInt() == 99);
    }

    SECTION("asInt returns default for non-numeric string")
    {
      auto const v = LayoutValue{std::string{"hello"}};
      int const defaultValue = 7;
      CHECK(v.asInt(defaultValue) == defaultValue);
    }

    SECTION("asBool coerces string true/false")
    {
      auto const vt = LayoutValue{std::string{"true"}};
      CHECK(vt.asBool() == true);

      auto const vf = LayoutValue{std::string{"false"}};
      CHECK(vf.asBool() == false);

      auto const vu = LayoutValue{std::string{"unknown"}};
      CHECK(vu.asBool() == false); // default
    }

    SECTION("asBool coerces int")
    {
      auto const v1 = LayoutValue{static_cast<std::int64_t>(1)};
      CHECK(v1.asBool() == true);

      auto const v0 = LayoutValue{static_cast<std::int64_t>(0)};
      CHECK(v0.asBool() == false);
    }

    SECTION("asDouble coerces string")
    {
      auto const v = LayoutValue{std::string{"3.14"}};
      CHECK(v.asDouble() == 3.14);
    }

    SECTION("asDouble returns default for non-numeric string")
    {
      auto const v = LayoutValue{std::string{"abc"}};
      double const defaultValue = 1.0;
      CHECK(v.asDouble(defaultValue) == defaultValue);
    }

    SECTION("asDouble coerces int")
    {
      auto const v = LayoutValue{static_cast<std::int64_t>(7)};
      CHECK(v.asDouble() == 7.0);
    }

    SECTION("monostate returns defaults")
    {
      auto const v = LayoutValue{};
      CHECK(v.asString("fallback") == "fallback");
      CHECK(v.asInt(42) == 42);
      CHECK(v.asBool(true) == true);
      CHECK(v.asDouble(1.5) == 1.5);
    }

    SECTION("vector string unwrapped via getIf")
    {
      auto const v = LayoutValue{std::vector<std::string>{"a", "b", "c"}};
      auto const* const vec = v.getIf<std::vector<std::string>>();
      REQUIRE(vec != nullptr);
      CHECK(vec->size() == 3);
      CHECK((*vec)[0] == "a");

      CHECK(v.getIf<bool>() == nullptr);
      CHECK(v.getIf<std::string>() == nullptr);
    }
  }

  // ---------------------------------------------------------------------------
  // Built-in Presets
  // ---------------------------------------------------------------------------
  TEST_CASE("Built-in preset validation", "[layout][presets]")
  {
    SECTION("Classic preset is the default and has no modern classes")
    {
      auto const doc = createDefaultLayout();
      CHECK(doc.root.getLayout<std::string>("cssClasses", "") != "ao-layout-preset-modern");
    }
  }

  // ---------------------------------------------------------------------------
  // LayoutNode helpers
  // ---------------------------------------------------------------------------
  TEST_CASE("LayoutNode getProp/getLayout", "[layout][model]")
  {
    auto node = LayoutNode{};
    node.props["label"] = LayoutValue{std::string{"hello"}};
    node.props["count"] = LayoutValue{static_cast<std::int64_t>(5)};
    node.props["enabled"] = LayoutValue{true};
    node.layout["hexpand"] = LayoutValue{true};
    node.layout["vexpand"] = LayoutValue{true};

    SECTION("getProp returns value when key exists")
    {
      CHECK(node.getProp<std::string>("label", "") == "hello");
      CHECK(node.getProp<std::int64_t>("count", 0) == 5);
      CHECK(node.getProp<bool>("enabled", false) == true);
    }

    SECTION("getProp returns default when key missing")
    {
      CHECK(node.getProp<std::string>("missing", "fallback") == "fallback");
      CHECK(node.getProp<std::int64_t>("missing", 99) == 99);
      CHECK(node.getProp<bool>("missing", true) == true);
    }

    SECTION("getLayout returns value when key exists")
    {
      CHECK(node.getLayout<bool>("hexpand", false) == true);
      CHECK(node.getLayout<bool>("vexpand", false) == true);
    }

    SECTION("getLayout returns default when key missing")
    {
      CHECK(node.getLayout<bool>("vexpand", true) == true);
      CHECK(node.getLayout<std::int64_t>("spacing", 10) == 10);
    }
  }
} // namespace ao::gtk::layout::test
