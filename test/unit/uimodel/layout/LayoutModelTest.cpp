// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutYaml.h> // NOLINT(misc-include-cleaner)
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::uimodel::layout::test
{
  namespace yaml = ao::yaml;

  TEST_CASE("LayoutValue serialization", "[layout][unit][model]")
  {
    SECTION("string value")
    {
      auto const v1 = LayoutValue{std::string{"hello"}};
      auto tree1 = ryml::Tree{};
      yaml::write(tree1.rootref(), v1);
      CHECK(yaml::scalarView(tree1.rootref()) == "hello");
    }

    SECTION("int value")
    {
      auto const v2 = LayoutValue{static_cast<std::int64_t>(42)};
      auto tree2 = ryml::Tree{};
      yaml::write(tree2.rootref(), v2);
      CHECK(yaml::asInt<std::int64_t>(tree2.rootref()) == 42);
    }

    SECTION("bool value")
    {
      auto const v3 = LayoutValue{true};
      auto tree3 = ryml::Tree{};
      yaml::write(tree3.rootref(), v3);
      CHECK(yaml::asBool(tree3.rootref()) == true);
    }

    SECTION("string list value")
    {
      auto const v4 = LayoutValue{std::vector<std::string>{"a", "b"}};
      auto tree4 = ryml::Tree{};
      yaml::write(tree4.rootref(), v4);
      auto const n4 = tree4.rootref();
      CHECK(n4.is_seq());
      CHECK(n4.num_children() == 2);
      CHECK(yaml::scalarView(n4[0]) == "a");
    }
  }

  TEST_CASE("LayoutNode round-trip", "[layout][unit][model]")
  {
    auto node = LayoutNode{};
    node.type = "box";
    node.id = "main";
    node.props["spacing"] = LayoutValue{static_cast<std::int64_t>(10)};

    auto child = LayoutNode{};
    child.type = "spacer";
    node.children.push_back(child);

    auto tree = ryml::Tree{};
    yaml::write(tree.rootref(), node);

    auto decoded = LayoutNode{};
    REQUIRE(yaml::read(tree.rootref(), decoded));

    CHECK(decoded.type == "box");
    CHECK(decoded.id == "main");
    CHECK(decoded.props.at("spacing").asInt() == 10);
    CHECK(decoded.children.size() == 1);
    CHECK(decoded.children[0].type == "spacer");
  }

  TEST_CASE("LayoutDocument round-trip preserves layout props and child order", "[layout][unit][model]")
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

  TEST_CASE("LayoutDocument round-trip preserves action-id props", "[layout][unit][model]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "playback.qualityIndicator";
    doc.root.props["primaryAction"] = LayoutValue{std::string{"playback.playPause"}};
    doc.root.props["secondaryAction"] = LayoutValue{std::string{"shell.showSystemMenu"}};

    auto tree = ryml::Tree{};
    yaml::write(tree.rootref(), doc);

    auto decoded = LayoutDocument{};
    REQUIRE(yaml::read(tree.rootref(), decoded));

    CHECK(decoded.root.type == "playback.qualityIndicator");
    CHECK(decoded.root.props.at("primaryAction").asString() == "playback.playPause");
    CHECK(decoded.root.props.at("secondaryAction").asString() == "shell.showSystemMenu");
  }

  TEST_CASE("LayoutDocument round-trip preserves tooltip", "[layout][unit][model]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "playback.qualityIndicator";

    auto tooltipNode = LayoutNode{};
    tooltipNode.type = "playback.audioPipelinePanel";
    tooltipNode.props["variant"] = LayoutValue{std::string{"tooltip"}};

    doc.root.optTooltip = BoxedLayoutNode{std::move(tooltipNode)};

    auto tree = ryml::Tree{};
    yaml::write(tree.rootref(), doc);

    auto decoded = LayoutDocument{};
    REQUIRE(yaml::read(tree.rootref(), decoded));

    CHECK(decoded.root.type == "playback.qualityIndicator");
    REQUIRE(decoded.root.optTooltip.has_value());
    REQUIRE(decoded.root.optTooltip->nodePtr != nullptr);
    CHECK(decoded.root.optTooltip->nodePtr->type == "playback.audioPipelinePanel");
    CHECK(decoded.root.optTooltip->nodePtr->props.at("variant").asString() == "tooltip");
  }

  TEST_CASE("YAML decode tolerates missing optional fields", "[layout][unit][model]")
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

  TEST_CASE("YAML decode tolerates fields set to empty string", "[layout][unit][model]")
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

  TEST_CASE("LayoutValue serializes and decodes double", "[layout][unit][model]")
  {
    auto const v = LayoutValue{3.14};
    auto tree = ryml::Tree{};
    yaml::write(tree.rootref(), v);

    auto decoded = LayoutValue{};
    REQUIRE(yaml::read(tree.rootref(), decoded));
    CHECK(decoded.asDouble() == 3.14);
  }

  TEST_CASE("LayoutValue coercion", "[layout][unit][model]")
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

  TEST_CASE("LayoutNode getProp/getLayout", "[layout][unit][model]")
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
} // namespace ao::uimodel::layout::test
