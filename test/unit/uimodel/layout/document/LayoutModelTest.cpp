// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    PreparedLayout preparedLayout(LayoutDocument const& document)
    {
      return ao::test::requireValue(prepareLayout(document));
    }
  } // namespace

  namespace yaml = ao::yaml;

  TEST_CASE("LayoutValue - serializes scalar variants without type loss", "[uimodel][unit][layout][document]")
  {
    SECTION("string value")
    {
      auto const v1 = LayoutValue{std::string{"hello"}};
      auto tree1 = ryml::Tree{};
      REQUIRE(writeLayoutValue(tree1.rootref(), v1));
      CHECK(yaml::scalarView(tree1.rootref()) == "hello");
    }

    SECTION("quoted string values retain their variant")
    {
      for (auto const value : {std::string_view{"true"}, std::string_view{"42"}, std::string_view{"null"}})
      {
        CAPTURE(value);
        auto tree = ryml::Tree{};
        REQUIRE(writeLayoutValue(tree.rootref(), LayoutValue{std::string{value}}));
        auto const emitted = ryml::emitrs_yaml<std::string>(tree);
        auto parsed = ryml::Tree{};
        ryml::parse_in_arena(ryml::to_csubstr(emitted), &parsed);

        auto decoded = readLayoutValue(parsed.rootref(), "test string");
        REQUIRE(decoded);
        REQUIRE(decoded->getIf<std::string>() != nullptr);
        CHECK(decoded->asString() == value);
      }
    }

    SECTION("int value")
    {
      auto const v2 = LayoutValue{static_cast<std::int64_t>(42)};
      auto tree2 = ryml::Tree{};
      REQUIRE(writeLayoutValue(tree2.rootref(), v2));
      CHECK(yaml::asInt<std::int64_t>(tree2.rootref()) == 42);
    }

    SECTION("bool value")
    {
      auto const v3 = LayoutValue{true};
      auto tree3 = ryml::Tree{};
      REQUIRE(writeLayoutValue(tree3.rootref(), v3));
      CHECK(yaml::scalarView(tree3.rootref()) == "true");
      CHECK(yaml::asBool(tree3.rootref()) == true);

      auto decoded = readLayoutValue(tree3.rootref(), "test value");
      REQUIRE(decoded);
      REQUIRE(decoded->getIf<bool>() != nullptr);
      CHECK(decoded->asBool() == true);

      auto const falseValue = LayoutValue{false};
      auto falseTree = ryml::Tree{};
      REQUIRE(writeLayoutValue(falseTree.rootref(), falseValue));
      CHECK(yaml::scalarView(falseTree.rootref()) == "false");
    }

    SECTION("string list value")
    {
      auto const v4 = LayoutValue{std::vector<std::string>{"a", "b"}};
      auto tree4 = ryml::Tree{};
      REQUIRE(writeLayoutValue(tree4.rootref(), v4));
      auto const n4 = tree4.rootref();
      CHECK(n4.is_seq());
      CHECK(n4.num_children() == 2);
      CHECK(yaml::scalarView(n4[0]) == "a");
    }
  }

  TEST_CASE("LayoutNode - round-trips identity and properties", "[uimodel][unit][layout][document]")
  {
    auto node = LayoutNode{};
    node.type = "box";
    node.id = "main";
    node.props["spacing"] = LayoutValue{static_cast<std::int64_t>(10)};

    auto child = LayoutNode{};
    child.type = "spacer";
    node.children.push_back(child);

    auto tree = ryml::Tree{};
    REQUIRE(writeLayoutNode(tree.rootref(), node));

    auto decoded = readLayoutNode(tree.rootref(), "test node");
    REQUIRE(decoded);

    CHECK(decoded->type == "box");
    CHECK(decoded->id == "main");
    CHECK(decoded->props.at("spacing").asInt() == 10);
    CHECK(decoded->children.size() == 1);
    CHECK(decoded->children[0].type == "spacer");
  }

  TEST_CASE("LayoutDocument - round-trip preserves layout props and child order", "[uimodel][unit][layout][document]")
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
    REQUIRE(LayoutDocumentYamlSchema{}.serialize(tree.rootref(), doc));

    auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decoded);

    REQUIRE(decoded->root.children.size() == 2);
    CHECK(decoded->root.children[0].type == "spacer");
    CHECK(decoded->root.children[0].layout.at("hexpand").asBool() == true);
    CHECK(decoded->root.children[0].layout.at("vexpand").asBool() == true);
    CHECK(decoded->root.children[1].type == "scroll");
    CHECK(decoded->root.children[1].id == "scroller");
    CHECK(decoded->root.children[1].layout.at("vexpand").asBool() == true);
    CHECK(decoded->root.children[1].props.at("hscrollPolicy").asString() == "never");
  }

  TEST_CASE("LayoutDocument - round-trip preserves action-id props", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "playback.qualityIndicator";
    doc.root.props["primaryAction"] = LayoutValue{std::string{"playback.playPause"}};
    doc.root.props["secondaryAction"] = LayoutValue{std::string{"shell.showSystemMenu"}};

    auto tree = ryml::Tree{};
    REQUIRE(LayoutDocumentYamlSchema{}.serialize(tree.rootref(), doc));

    auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decoded);

    CHECK(decoded->root.type == "playback.qualityIndicator");
    CHECK(decoded->root.props.at("primaryAction").asString() == "playback.playPause");
    CHECK(decoded->root.props.at("secondaryAction").asString() == "shell.showSystemMenu");
  }

  TEST_CASE("LayoutDocument - round-trip preserves tooltip", "[uimodel][unit][layout][document]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "playback.qualityIndicator";

    auto tooltipNode = LayoutNode{};
    tooltipNode.type = "playback.audioPipelinePanel";
    tooltipNode.props["variant"] = LayoutValue{std::string{"tooltip"}};

    doc.root.optTooltip = BoxedLayoutNode{std::move(tooltipNode)};

    auto tree = ryml::Tree{};
    REQUIRE(LayoutDocumentYamlSchema{}.serialize(tree.rootref(), doc));

    auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decoded);

    CHECK(decoded->root.type == "playback.qualityIndicator");
    REQUIRE(decoded->root.optTooltip);
    REQUIRE(decoded->root.optTooltip->nodePtr != nullptr);
    CHECK(decoded->root.optTooltip->nodePtr->type == "playback.audioPipelinePanel");
    CHECK(decoded->root.optTooltip->nodePtr->props.at("variant").asString() == "tooltip");
  }

  TEST_CASE("LayoutModel - YAML deserialization tolerates missing optional fields", "[uimodel][unit][layout][document]")
  {
    auto const* source = R"(
      version: 1
      root:
        type: box
    )";
    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

    auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decoded);
    CHECK(decoded->version == 1);
    CHECK(decoded->root.type == "box");
    CHECK(decoded->root.id.empty());
    CHECK(decoded->root.children.empty());
    CHECK(decoded->root.props.empty());
  }

  TEST_CASE("LayoutModel - YAML deserialization tolerates fields set to empty string",
            "[uimodel][unit][layout][document]")
  {
    auto const* source = R"(
      version: 1
      root:
        id: ""
        type: spacer
    )";
    auto tree = ryml::Tree{yaml::callbacks()};
    ryml::parse_in_arena(ryml::to_csubstr(source), &tree);

    auto decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});
    REQUIRE(decoded);
    CHECK(decoded->root.type == "spacer");
    CHECK(decoded->root.id.empty());
  }

  TEST_CASE("LayoutDocumentYamlSchema - rejects malformed and unsupported documents", "[uimodel][unit][layout][schema]")
  {
    SECTION("Unsupported version is reported before interpreting its payload")
    {
      auto const* source = R"(
        version: 99
        root: not-a-node
        futureRootField: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::NotSupported);
    }

    SECTION("Unknown structural fields are rejected")
    {
      auto const* source = R"(
        version: 1
        root:
          type: box
        unexpected: true
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("unexpected"));
    }

    SECTION("Missing required node type is rejected")
    {
      auto const* source = R"(
        version: 1
        root:
          id: root
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("type"));
    }

    SECTION("Every scalar-list item must be a scalar")
    {
      auto const* source = R"(
        version: 1
        root:
          type: box
          layout:
            cssClasses:
              - valid
              - nested: invalid
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(source), &tree);
      auto const decoded = LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), LayoutDocument{});

      REQUIRE_FALSE(decoded);
      CHECK(decoded.error().code == Error::Code::FormatRejected);
      CHECK(decoded.error().message.contains("cssClasses"));
    }
  }

  TEST_CASE("LayoutValue - serializes and deserializes double", "[uimodel][unit][layout][document]")
  {
    auto const v = LayoutValue{3.14};
    auto tree = ryml::Tree{};
    REQUIRE(writeLayoutValue(tree.rootref(), v));

    auto decoded = readLayoutValue(tree.rootref(), "test double");
    REQUIRE(decoded);
    CHECK(decoded->asDouble() == 3.14);
  }

  TEST_CASE("LayoutValue - coercion returns typed optional values", "[uimodel][unit][layout][document]")
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

    SECTION("asInt rejects partially numeric string")
    {
      auto const v = LayoutValue{std::string{"99px"}};
      CHECK(v.asInt(7) == 7);
    }

    SECTION("asBool coerces string true/false")
    {
      auto const vt = LayoutValue{std::string{"true"}};
      CHECK(vt.asBool() == true);

      auto const vf = LayoutValue{std::string{"false"}};
      CHECK(vf.asBool() == false);

      auto const vu = LayoutValue{std::string{"unknown"}};
      CHECK(vu.asBool() == false);
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

    SECTION("asDouble rejects partially numeric string")
    {
      auto const v = LayoutValue{std::string{"3.14px"}};
      CHECK(v.asDouble(1.0) == 1.0);
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

  TEST_CASE("LayoutNode - property lookups distinguish props from layout props", "[uimodel][unit][layout][document]")
  {
    auto node = LayoutNode{};
    node.props["label"] = LayoutValue{std::string{"hello"}};
    node.props["count"] = LayoutValue{static_cast<std::int64_t>(5)};
    node.props["enabled"] = LayoutValue{true};
    node.layout["hexpand"] = LayoutValue{true};
    node.layout["vexpand"] = LayoutValue{true};

    SECTION("propertyOr returns value when key exists")
    {
      CHECK(node.propertyOr<std::string>("label", "") == "hello");
      CHECK(node.propertyOr<std::int64_t>("count", 0) == 5);
      CHECK(node.propertyOr<bool>("enabled", false) == true);
    }

    SECTION("propertyOr returns default when key missing")
    {
      CHECK(node.propertyOr<std::string>("missing", "fallback") == "fallback");
      CHECK(node.propertyOr<std::int64_t>("missing", 99) == 99);
      CHECK(node.propertyOr<bool>("missing", true) == true);
    }

    SECTION("layoutOr returns value when key exists")
    {
      CHECK(node.layoutOr<bool>("hexpand", false) == true);
      CHECK(node.layoutOr<bool>("vexpand", false) == true);
    }

    SECTION("layoutOr returns default when key missing")
    {
      CHECK(node.layoutOr<bool>("vexpand", true) == true);
      CHECK(node.layoutOr<std::int64_t>("spacing", 10) == 10);
    }
  }

  TEST_CASE("LayoutModel - layout stateful node ids reject ambiguous runtime keys", "[uimodel][unit][regression]")
  {
    SECTION("duplicate stateful ids are errors")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {
        LayoutNode{.id = "shared-split", .type = "split"},
        LayoutNode{.id = "shared-split", .type = "collapsibleSplit"},
      };

      auto const diagnostics = validateStatefulLayoutNodeIds(preparedLayout(doc));

      REQUIRE(diagnostics.size() == 1);
      CHECK(diagnostics[0].severity == LayoutNodeIdDiagnosticSeverity::Error);
      CHECK(diagnostics[0].componentId == "shared-split");
      CHECK(hasLayoutNodeIdErrors(diagnostics));
    }

    SECTION("anonymous stateful nodes warn but remain valid")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";

      auto const diagnostics = validateStatefulLayoutNodeIds(preparedLayout(doc));

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

      CHECK(validateStatefulLayoutNodeIds(preparedLayout(doc)).empty());
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

      auto const diagnostics = validateStatefulLayoutNodeIds(preparedLayout(doc));

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

      auto const diagnostics = validateStatefulLayoutNodeIds(preparedLayout(doc));
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

      CHECK(validateStatefulLayoutNodeIds(preparedLayout(doc)).empty());
    }
  }

  TEST_CASE("LayoutModel - layout stateful node id generation avoids existing document ids",
            "[uimodel][unit][layout][document]")
  {
    SECTION("new stateful ids are stable and unique across root and templates")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children = {LayoutNode{.id = "split-new", .type = "split"}};
      doc.templates["copy"] = LayoutNode{.id = "split-new-2", .type = "split"};

      CHECK(makeUniqueLayoutNodeId(doc, "split", "new") == "split-new-3");
    }

    SECTION("regenerating a copied stateful subtree replaces ids recursively")
    {
      auto owner = LayoutDocument{};
      owner.root.type = "box";
      owner.root.children = {LayoutNode{.id = "split-original", .type = "split"}};

      auto copy = LayoutNode{.id = "split-original", .type = "split"};
      copy.children = {LayoutNode{.id = "child", .type = "spacer"}, LayoutNode{.type = "collapsibleSplit"}};

      regenerateLayoutNodeIds(copy, owner);

      CHECK(copy.id != "split-original");
      CHECK(copy.id == "split-split-original");
      REQUIRE(copy.children.size() == 2);
      CHECK(copy.children[0].id == "spacer-child");
      CHECK(copy.children[1].id == "collapsiblesplit-copy");
    }
  }
} // namespace ao::uimodel::test
