// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateYaml.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

namespace ao::uimodel::test
{
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
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(node),
        .state = {{"positionPercent", LayoutValue{0.42}}},
      };
      return doc;
    }
  } // namespace

  TEST_CASE("LayoutComponentState - file round-trips entries", "[uimodel][unit][layout][component]")
  {
    auto const node = splitNode();
    auto original = stateDocFor(node);

    auto tree = ryml::Tree{};
    yaml::write(tree.rootref(), original);

    auto decoded = LayoutComponentStateDocument{};
    REQUIRE(yaml::read(tree.rootref(), decoded));

    CHECK(decoded.version == kStateFileVersion);
    CHECK(decoded.preset == "modern");
    REQUIRE(decoded.components.contains("main-paned"));
    CHECK(decoded.components.at("main-paned").type == "split");
    CHECK(decoded.components.at("main-paned").state.at("positionPercent").asDouble() == 0.42);
  }

  TEST_CASE("LayoutComponentState - resolver validates versions type and baseline",
            "[uimodel][unit][layout][component]")
  {
    auto node = splitNode();
    auto stateDoc = stateDocFor(node);

    SECTION("matching state resolves")
    {
      auto const optResolved = resolveComponentState(stateDoc, node);
      REQUIRE(optResolved);
      CHECK(optResolved->state.at("positionPercent").asDouble() == 0.42);
    }

    SECTION("unknown file version is ignored")
    {
      stateDoc.version = 99;
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("unknown component state version is ignored")
    {
      stateDoc.components.at(node.id).stateVersion = 99;
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("type mismatch is ignored")
    {
      stateDoc.components.at(node.id).type = "collapsibleSplit";
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("baseline mismatch is ignored")
    {
      node.props["resizeStart"] = LayoutValue{false};
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }

    SECTION("anonymous nodes are never resolved")
    {
      node.id.clear();
      CHECK_FALSE(resolveComponentState(stateDoc, node).has_value());
    }
  }

  TEST_CASE("LayoutComponentState - layout component baseline hash ignores non-semantic document changes",
            "[uimodel][unit][layout][component]")
  {
    SECTION("equivalent numeric spellings hash the same")
    {
      auto first = splitNode();
      first.props["initialPositionPercent"] = LayoutValue{0.2};

      auto second = splitNode();
      second.props["initialPositionPercent"] = LayoutValue{std::string{"0.20"}};

      CHECK(componentBaselineHash(first) == componentBaselineHash(second));
    }

    SECTION("irrelevant children do not change parent hash")
    {
      auto first = splitNode();
      auto second = first;
      second.children.push_back(LayoutNode{.id = "extra-child", .type = "separator"});

      CHECK(componentBaselineHash(first) == componentBaselineHash(second));
    }

    SECTION("relevant prop edits change the hash")
    {
      auto first = splitNode();
      auto second = first;
      second.props["orientation"] = LayoutValue{std::string{"vertical"}};

      CHECK(componentBaselineHash(first) != componentBaselineHash(second));
    }
  }

  TEST_CASE("LayoutComponentState - YAML decode handles corrupt files", "[uimodel][unit][layout][component]")
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

  TEST_CASE("LayoutComponentState - pruning removes invalid entries", "[uimodel][unit][layout][component]")
  {
    auto liveNode = splitNode("live-split");
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children = {liveNode};

    auto stateDoc = LayoutComponentStateDocument{};
    stateDoc.preset = "classic";
    stateDoc.components["live-split"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(liveNode),
      .state = {{"positionPercent", LayoutValue{0.25}}},
    };
    stateDoc.components["deleted-split"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kStateEntryVersion,
      .baselineHash = "orphan",
      .state = {{"positionPercent", LayoutValue{0.50}}},
    };
    stateDoc.components["wrong-type"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(liveNode),
      .state = {{"positionPercent", LayoutValue{0.75}}},
    };
    doc.root.children.push_back(LayoutNode{.id = "wrong-type", .type = "split"});

    pruneComponentState(stateDoc, doc);

    CHECK(stateDoc.components.size() == 1);
    CHECK(stateDoc.components.contains("live-split"));
  }
} // namespace ao::uimodel::test
