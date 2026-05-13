// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <app/linux-gtk/layout/document/LayoutDocument.h>
#include <app/linux-gtk/layout/document/LayoutYaml.h>

#include <catch2/catch_test_macros.hpp>
#include <yaml-cpp/yaml.h>

using namespace ao::gtk::layout;

// ---------------------------------------------------------------------------
// YAML serialization
// ---------------------------------------------------------------------------

TEST_CASE("Layout model serialization", "[layout][model]")
{
  SECTION("LayoutValue serialization")
  {
    auto const v1 = LayoutValue{std::string{"hello"}};
    auto const n1 = YAML::Node(v1);
    CHECK(n1.as<std::string>() == "hello");

    auto const v2 = LayoutValue{static_cast<std::int64_t>(42)};
    auto const n2 = YAML::Node(v2);
    CHECK(n2.as<std::int64_t>() == 42);

    auto const v3 = LayoutValue{true};
    auto const n3 = YAML::Node(v3);
    CHECK(n3.as<bool>() == true);

    auto const v4 = LayoutValue{std::vector<std::string>{"a", "b"}};
    auto const n4 = YAML::Node(v4);
    CHECK(n4.IsSequence());
    CHECK(n4.size() == 2);
    CHECK(n4[0].as<std::string>() == "a");
  }

  SECTION("LayoutNode round-trip")
  {
    auto node = LayoutNode{};
    node.type = "box";
    node.id = "main";
    node.props["spacing"] = LayoutValue{static_cast<std::int64_t>(10)};

    auto child = LayoutNode{};
    child.type = "spacer";
    node.children.push_back(child);

    auto const yaml = YAML::Node(node);
    auto const decoded = yaml.as<LayoutNode>();

    CHECK(decoded.type == "box");
    CHECK(decoded.id == "main");
    CHECK(decoded.props.at("spacing").asInt() == 10);
    CHECK(decoded.children.size() == 1);
    CHECK(decoded.children[0].type == "spacer");
  }

  SECTION("LayoutDocument round-trip")
  {
    auto const doc = createDefaultLayout();
    auto const yaml = YAML::Node(doc);
    auto const decoded = yaml.as<LayoutDocument>();

    CHECK(decoded.version == 1);
    CHECK(decoded.root.type == "box");
    REQUIRE(decoded.root.children.size() >= 1);
    CHECK(decoded.root.children[0].type == "app.menuBar");
  }

  SECTION("LayoutDocument round-trip preserves layout props and child order")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.id = "root";

    auto c1 = LayoutNode{};
    c1.type = "spacer";
    c1.layout["hexpand"] = LayoutValue{true};
    c1.layout["margin"] = LayoutValue{static_cast<std::int64_t>(4)};
    doc.root.children.push_back(c1);

    auto c2 = LayoutNode{};
    c2.type = "scroll";
    c2.id = "scroller";
    c2.layout["vexpand"] = LayoutValue{true};
    c2.props["hscrollPolicy"] = LayoutValue{std::string{"never"}};
    doc.root.children.push_back(c2);

    auto const yaml = YAML::Node(doc);
    auto const decoded = yaml.as<LayoutDocument>();

    REQUIRE(decoded.root.children.size() == 2);
    CHECK(decoded.root.children[0].type == "spacer");
    CHECK(decoded.root.children[0].layout.at("hexpand").asBool() == true);
    CHECK(decoded.root.children[0].layout.at("margin").asInt() == 4);
    CHECK(decoded.root.children[1].type == "scroll");
    CHECK(decoded.root.children[1].id == "scroller");
    CHECK(decoded.root.children[1].layout.at("vexpand").asBool() == true);
    CHECK(decoded.root.children[1].props.at("hscrollPolicy").asString() == "never");
  }

  SECTION("YAML decode tolerates missing optional fields")
  {
    auto const node = YAML::Load(R"(
      version: 1
      root:
        type: box
    )");

    auto const decoded = node.as<LayoutDocument>();
    CHECK(decoded.version == 1);
    CHECK(decoded.root.type == "box");
    CHECK(decoded.root.id.empty());
    CHECK(decoded.root.children.empty());
    CHECK(decoded.root.props.empty());
  }

  SECTION("YAML decode tolerates fields set to empty string")
  {
    auto const node = YAML::Load(R"(
      version: 1
      root:
        id: ""
        type: spacer
    )");

    auto const decoded = node.as<LayoutDocument>();
    CHECK(decoded.root.type == "spacer");
    CHECK(decoded.root.id.empty());
  }

  SECTION("LayoutValue serializes and decodes double")
  {
    auto const v = LayoutValue{3.14};
    auto const n = YAML::Node(v);
    CHECK(n.as<double>() == 3.14);
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
// LayoutNode helpers
// ---------------------------------------------------------------------------

TEST_CASE("LayoutNode getProp/getLayout", "[layout][model]")
{
  auto node = LayoutNode{};
  node.props["label"] = LayoutValue{std::string{"hello"}};
  node.props["count"] = LayoutValue{static_cast<std::int64_t>(5)};
  node.props["enabled"] = LayoutValue{true};
  node.layout["hexpand"] = LayoutValue{true};
  node.layout["margin"] = LayoutValue{static_cast<std::int64_t>(8)};

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
    CHECK(node.getLayout<std::int64_t>("margin", 0) == 8);
  }

  SECTION("getLayout returns default when key missing")
  {
    CHECK(node.getLayout<bool>("vexpand", true) == true);
    CHECK(node.getLayout<std::int64_t>("spacing", 10) == 10);
  }
}
