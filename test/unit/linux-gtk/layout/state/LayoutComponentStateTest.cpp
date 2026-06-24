// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutComponentStateYaml.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  namespace yaml = ao::yaml;

  namespace
  {
    LayoutNode splitNode(std::string id = "main-paned")
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = "split";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["initialPositionPercent"] = LayoutValue{0.2};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }

    LayoutComponentStateDocument stateDocFor(LayoutNode const& node)
    {
      auto doc = LayoutComponentStateDocument{};
      doc.preset = "modern";
      doc.components[node.id] = LayoutComponentStateEntry{
        .type = node.type,
        .stateVersion = kLayoutComponentStateEntryVersion,
        .baselineHash = layoutComponentBaselineHash(node),
        .state = {{"positionPercent", LayoutValue{0.42}}},
      };
      return doc;
    }
  } // namespace

  TEST_CASE("Layout component state file round-trip", "[layout][unit][state]")
  {
    auto const node = splitNode();
    auto original = stateDocFor(node);

    auto tree = ryml::Tree{};
    yaml::write(tree.rootref(), original);

    auto decoded = LayoutComponentStateDocument{};
    REQUIRE(yaml::read(tree.rootref(), decoded));

    CHECK(decoded.version == kLayoutComponentStateFileVersion);
    CHECK(decoded.preset == "modern");
    REQUIRE(decoded.components.contains("main-paned"));
    CHECK(decoded.components.at("main-paned").type == "split");
    CHECK(decoded.components.at("main-paned").state.at("positionPercent").asDouble() == 0.42);
  }

  TEST_CASE("Layout component state resolver validates versions type and baseline", "[layout][unit][state]")
  {
    auto node = splitNode();
    auto stateDoc = stateDocFor(node);

    SECTION("matching state resolves")
    {
      auto const optResolved = resolveLayoutComponentState(stateDoc, node);
      REQUIRE(optResolved.has_value());
      CHECK(optResolved->state.at("positionPercent").asDouble() == 0.42);
    }

    SECTION("unknown file version is ignored")
    {
      stateDoc.version = 99;
      CHECK_FALSE(resolveLayoutComponentState(stateDoc, node).has_value());
    }

    SECTION("unknown component state version is ignored")
    {
      stateDoc.components.at(node.id).stateVersion = 99;
      CHECK_FALSE(resolveLayoutComponentState(stateDoc, node).has_value());
    }

    SECTION("type mismatch is ignored")
    {
      stateDoc.components.at(node.id).type = "collapsibleSplit";
      CHECK_FALSE(resolveLayoutComponentState(stateDoc, node).has_value());
    }

    SECTION("baseline mismatch is ignored")
    {
      node.props["resizeStart"] = LayoutValue{false};
      CHECK_FALSE(resolveLayoutComponentState(stateDoc, node).has_value());
    }

    SECTION("anonymous nodes are never resolved")
    {
      node.id.clear();
      CHECK_FALSE(resolveLayoutComponentState(stateDoc, node).has_value());
    }
  }

  TEST_CASE("Layout component baseline hash is semantic", "[layout][unit][state]")
  {
    SECTION("equivalent numeric spellings hash the same")
    {
      auto first = splitNode();
      first.props["initialPositionPercent"] = LayoutValue{0.2};

      auto second = splitNode();
      second.props["initialPositionPercent"] = LayoutValue{std::string{"0.20"}};

      CHECK(layoutComponentBaselineHash(first) == layoutComponentBaselineHash(second));
    }

    SECTION("irrelevant children do not change parent hash")
    {
      auto first = splitNode();
      auto second = first;
      second.children.push_back(LayoutNode{.id = "extra-child", .type = "separator"});

      CHECK(layoutComponentBaselineHash(first) == layoutComponentBaselineHash(second));
    }

    SECTION("relevant prop edits change the hash")
    {
      auto first = splitNode();
      auto second = first;
      second.props["orientation"] = LayoutValue{std::string{"vertical"}};

      CHECK(layoutComponentBaselineHash(first) != layoutComponentBaselineHash(second));
    }
  }

  TEST_CASE("Layout component state YAML decode handles corrupt files", "[layout][unit][state]")
  {
    SECTION("malformed state root is rejected")
    {
      auto const* text = "not-a-map";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);

      auto decoded = LayoutComponentStateDocument{};
      CHECK_FALSE(yaml::read(tree.rootref(), decoded));
    }

    SECTION("malformed component entries are skipped")
    {
      auto const* text = R"(
        version: 1
        preset: modern
        components:
          bad-entry:
            type: split
            stateVersion: 1
          valid-entry:
            type: split
            stateVersion: 1
            baselineHash: abc123
            state:
              positionPercent: 0.33
      )";
      auto tree = ryml::Tree{yaml::callbacks()};
      ryml::parse_in_arena(ryml::to_csubstr(text), &tree);

      auto decoded = LayoutComponentStateDocument{};
      REQUIRE(yaml::read(tree.rootref(), decoded));
      CHECK_FALSE(decoded.components.contains("bad-entry"));
      REQUIRE(decoded.components.contains("valid-entry"));
      CHECK(decoded.components.at("valid-entry").state.at("positionPercent").asDouble() == 0.33);
    }
  }

  TEST_CASE("Layout component state pruning removes invalid entries", "[layout][unit][state]")
  {
    auto liveNode = splitNode("live-split");
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children = {liveNode};

    auto stateDoc = LayoutComponentStateDocument{};
    stateDoc.preset = "classic";
    stateDoc.components["live-split"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = layoutComponentBaselineHash(liveNode),
      .state = {{"positionPercent", LayoutValue{0.25}}},
    };
    stateDoc.components["deleted-split"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = "orphan",
      .state = {{"positionPercent", LayoutValue{0.50}}},
    };
    stateDoc.components["wrong-type"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = layoutComponentBaselineHash(liveNode),
      .state = {{"positionPercent", LayoutValue{0.75}}},
    };
    doc.root.children.push_back(LayoutNode{.id = "wrong-type", .type = "split"});

    pruneLayoutComponentState(stateDoc, doc);

    REQUIRE(stateDoc.components.size() == 1);
    CHECK(stateDoc.components.contains("live-split"));
  }
} // namespace ao::gtk::layout::test
