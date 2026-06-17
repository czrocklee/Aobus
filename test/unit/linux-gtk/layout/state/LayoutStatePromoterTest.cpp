// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutStatePromoter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  namespace
  {
    LayoutNode splitNode(std::string id)
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = "split";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["position"] = LayoutValue{static_cast<std::int64_t>(200)};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }

    LayoutNode collapsibleSplitNode(std::string id)
    {
      auto node = LayoutNode{};
      node.id = std::move(id);
      node.type = "collapsibleSplit";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["position"] = LayoutValue{static_cast<std::int64_t>(150)};
      node.props["initialPositionPercent"] = LayoutValue{0.25};
      node.props["revealed"] = LayoutValue{true};
      node.children = {LayoutNode{.type = "spacer"}, LayoutNode{.type = "spacer"}};
      return node;
    }
  } // namespace

  TEST_CASE("LayoutStatePromoter promotes runtime panel sizes", "[layout][unit][state]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(splitNode("main-paned"));
    doc.root.children.push_back(collapsibleSplitNode("detail-split"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["main-paned"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = layoutComponentBaselineHash(doc.root.children[0]),
      .state = {{"positionPercent", LayoutValue{0.42}}},
    };
    stateDoc.components["detail-split"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = layoutComponentBaselineHash(doc.root.children[1]),
      .state = {{"size", LayoutValue{static_cast<std::int64_t>(320)}}, {"revealed", LayoutValue{false}}},
    };

    auto const result = promotePanelSizeDefaults(doc, stateDoc);

    CHECK(result.changed);
    CHECK(result.promotedCount == 2);
    CHECK(result.residualCount == 1);

    auto const& split = doc.root.children[0];
    CHECK(split.props.find("position") == split.props.end());
    CHECK(split.props.at("initialPositionPercent").asDouble() == 0.42);

    auto const& collapsible = doc.root.children[1];
    CHECK(collapsible.props.at("position").asInt() == 320);
    CHECK(collapsible.props.find("initialPositionPercent") == collapsible.props.end());

    CHECK_FALSE(stateDoc.components.contains("main-paned"));
    REQUIRE(stateDoc.components.contains("detail-split"));
    CHECK(stateDoc.components.at("detail-split").state.size() == 1);
    CHECK(stateDoc.components.at("detail-split").state.at("revealed").asBool(true) == false);
    CHECK(stateDoc.components.at("detail-split").baselineHash == layoutComponentBaselineHash(doc.root.children[1]));
  }

  TEST_CASE("LayoutStatePromoter no-op when no matching state", "[layout][unit][state]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(splitNode("main-paned"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};

    auto const result = promotePanelSizeDefaults(doc, stateDoc);

    CHECK_FALSE(result.changed);
    CHECK(result.promotedCount == 0);
    CHECK(result.residualCount == 0);
  }

  TEST_CASE("LayoutStatePromoter promotes deep layout trees", "[layout][unit][state]")
  {
    auto doc = LayoutDocument{};
    auto deepNode = collapsibleSplitNode("deep-split");

    auto wrapper1 = LayoutNode{};
    wrapper1.type = "box";
    wrapper1.children.push_back(std::move(deepNode));

    auto wrapper2 = LayoutNode{};
    wrapper2.type = "box";
    wrapper2.children.push_back(std::move(wrapper1));

    doc.root.type = "box";
    doc.root.children.push_back(std::move(wrapper2));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["deep-split"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = layoutComponentBaselineHash(doc.root.children[0].children[0].children[0]),
      .state = {{"size", LayoutValue{static_cast<std::int64_t>(320)}}},
    };

    auto const result = promotePanelSizeDefaults(doc, stateDoc);
    CHECK(result.changed);
    CHECK(result.promotedCount == 1);
  }

  TEST_CASE("LayoutStatePromoter rejects mismatched baseline hash", "[layout][unit][state]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(splitNode("main-paned"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["main-paned"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = "bad_hash",
      .state = {{"positionPercent", LayoutValue{0.42}}},
    };

    auto const result = promotePanelSizeDefaults(doc, stateDoc);
    CHECK_FALSE(result.changed);
    CHECK(result.promotedCount == 0);
  }

  TEST_CASE("LayoutStatePromoter gracefully ignores malformed state types", "[layout][unit][state]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(collapsibleSplitNode("detail-split"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["detail-split"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kLayoutComponentStateEntryVersion,
      .baselineHash = layoutComponentBaselineHash(doc.root.children[0]),
      .state = {{"size", LayoutValue{true}}},
    };

    auto const result = promotePanelSizeDefaults(doc, stateDoc);
    CHECK_FALSE(result.changed);
    CHECK(result.promotedCount == 0);
  }
} // namespace ao::gtk::layout::test
