// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutStatePromoter.h>

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    void checkValuesEqual(LayoutValueMap const& actual, LayoutValueMap const& expected)
    {
      REQUIRE(actual.size() == expected.size());

      for (auto const& [key, value] : expected)
      {
        REQUIRE(actual.contains(key));
        CHECK(actual.at(key).data == value.data);
      }
    }

    void checkNodeEqual(LayoutNode const& actual, LayoutNode const& expected)
    {
      CHECK(actual.id == expected.id);
      CHECK(actual.type == expected.type);
      checkValuesEqual(actual.props, expected.props);
      checkValuesEqual(actual.layout, expected.layout);
      REQUIRE(actual.children.size() == expected.children.size());

      for (std::size_t index = 0; index < expected.children.size(); ++index)
      {
        checkNodeEqual(actual.children[index], expected.children[index]);
      }

      REQUIRE(actual.optTooltip.has_value() == expected.optTooltip.has_value());

      if (expected.optTooltip)
      {
        REQUIRE(static_cast<bool>(actual.optTooltip->nodePtr) == static_cast<bool>(expected.optTooltip->nodePtr));

        if (expected.optTooltip->nodePtr)
        {
          checkNodeEqual(*actual.optTooltip->nodePtr, *expected.optTooltip->nodePtr);
        }
      }
    }

    void checkDocumentEqual(LayoutDocument const& actual, LayoutDocument const& expected)
    {
      CHECK(actual.version == expected.version);
      checkNodeEqual(actual.root, expected.root);
      REQUIRE(actual.templates.size() == expected.templates.size());

      for (auto const& [id, node] : expected.templates)
      {
        REQUIRE(actual.templates.contains(id));
        checkNodeEqual(actual.templates.at(id), node);
      }
    }

    void checkStateEqual(LayoutComponentStateDocument const& actual, LayoutComponentStateDocument const& expected)
    {
      CHECK(actual.version == expected.version);
      CHECK(actual.preset == expected.preset);
      REQUIRE(actual.components.size() == expected.components.size());

      for (auto const& [id, entry] : expected.components)
      {
        REQUIRE(actual.components.contains(id));
        auto const& actualEntry = actual.components.at(id);
        CHECK(actualEntry.type == entry.type);
        CHECK(actualEntry.stateVersion == entry.stateVersion);
        CHECK(actualEntry.baselineHash == entry.baselineHash);
        checkValuesEqual(actualEntry.state, entry.state);
      }
    }

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

  TEST_CASE("LayoutStatePromoter - promotes runtime panel sizes", "[uimodel][unit][layout][component]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(splitNode("main-paned"));
    doc.root.children.push_back(collapsibleSplitNode("detail-split"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["main-paned"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(doc.root.children[0]),
      .state = {{"positionPercent", LayoutValue{0.42}}},
    };
    stateDoc.components["detail-split"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(doc.root.children[1]),
      .state = {{"size", LayoutValue{static_cast<std::int64_t>(320)}}, {"revealed", LayoutValue{false}}},
    };

    auto const changed = tryPromotePanelSizeDefaults(doc, stateDoc);

    CHECK(changed);
    REQUIRE(doc.root.children.size() == 2);

    auto const& split = doc.root.children[0];
    CHECK_FALSE(split.props.contains("position"));
    CHECK(split.props.at("initialPositionPercent").asDouble() == 0.42);

    auto const& collapsible = doc.root.children[1];
    CHECK(collapsible.props.at("position").asInt() == 320);
    CHECK_FALSE(collapsible.props.contains("initialPositionPercent"));

    CHECK_FALSE(stateDoc.components.contains("main-paned"));
    REQUIRE(stateDoc.components.contains("detail-split"));
    CHECK(stateDoc.components.at("detail-split").state.size() == 1);
    CHECK(stateDoc.components.at("detail-split").state.at("revealed").asBool(true) == false);
    CHECK(stateDoc.components.at("detail-split").baselineHash == componentBaselineHash(doc.root.children[1]));
  }

  TEST_CASE("LayoutStatePromoter - clamps promoted panel sizes", "[uimodel][unit][layout][component]")
  {
    SECTION("split percentages clamp to zero")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children.push_back(splitNode("main-paned"));

      auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["main-paned"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(doc.root.children[0]),
        .state = {{"positionPercent", LayoutValue{-0.25}}},
      };

      REQUIRE(tryPromotePanelSizeDefaults(doc, stateDoc));
      REQUIRE(doc.root.children.size() == 1);
      CHECK(doc.root.children[0].props.at("initialPositionPercent").asDouble() == 0.0);
      CHECK_FALSE(stateDoc.components.contains("main-paned"));
    }

    SECTION("split percentages clamp to one")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children.push_back(splitNode("main-paned"));

      auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["main-paned"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(doc.root.children[0]),
        .state = {{"positionPercent", LayoutValue{1.25}}},
      };

      REQUIRE(tryPromotePanelSizeDefaults(doc, stateDoc));
      REQUIRE(doc.root.children.size() == 1);
      CHECK(doc.root.children[0].props.at("initialPositionPercent").asDouble() == 1.0);
      CHECK_FALSE(stateDoc.components.contains("main-paned"));
    }

    SECTION("collapsible sizes clamp to fifty")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children.push_back(collapsibleSplitNode("detail-split"));

      auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
      stateDoc.components["detail-split"] = LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(doc.root.children[0]),
        .state = {{"size", LayoutValue{static_cast<std::int64_t>(20)}}},
      };

      REQUIRE(tryPromotePanelSizeDefaults(doc, stateDoc));
      REQUIRE(doc.root.children.size() == 1);
      CHECK(doc.root.children[0].props.at("position").asInt() == 50);
      CHECK_FALSE(stateDoc.components.contains("detail-split"));
    }
  }

  TEST_CASE("LayoutStatePromoter - ignores documents with no matching state", "[uimodel][unit][layout][component]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(splitNode("main-paned"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    auto const beforeDocument = doc;
    auto const beforeState = stateDoc;

    auto const changed = tryPromotePanelSizeDefaults(doc, stateDoc);

    CHECK_FALSE(changed);
    checkDocumentEqual(doc, beforeDocument);
    checkStateEqual(stateDoc, beforeState);
    CHECK(doc.root.children[0].props.at("position").asInt() == 200);
    CHECK(stateDoc.components.empty());
  }

  TEST_CASE("LayoutStatePromoter - promotes deep layout trees", "[uimodel][unit][layout][component]")
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
      .stateVersion = kStateEntryVersion,
      .baselineHash = componentBaselineHash(doc.root.children[0].children[0].children[0]),
      .state = {{"size", LayoutValue{static_cast<std::int64_t>(320)}}},
    };

    REQUIRE(tryPromotePanelSizeDefaults(doc, stateDoc));
    REQUIRE(doc.root.children.size() == 1);
    REQUIRE(doc.root.children[0].children.size() == 1);
    REQUIRE(doc.root.children[0].children[0].children.size() == 1);

    auto const& promoted = doc.root.children[0].children[0].children[0];
    CHECK(promoted.props.at("position").asInt() == 320);
    CHECK_FALSE(promoted.props.contains("initialPositionPercent"));
    CHECK_FALSE(stateDoc.components.contains("deep-split"));
  }

  TEST_CASE("LayoutStatePromoter - rejects mismatched baseline hash", "[uimodel][unit][layout][component]")
  {
    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(splitNode("main-paned"));

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["main-paned"] = LayoutComponentStateEntry{
      .type = "split",
      .stateVersion = kStateEntryVersion,
      .baselineHash = "bad_hash",
      .state = {{"positionPercent", LayoutValue{0.42}}},
    };

    auto const beforeDocument = doc;
    auto const beforeState = stateDoc;
    CHECK_FALSE(tryPromotePanelSizeDefaults(doc, stateDoc));
    checkDocumentEqual(doc, beforeDocument);
    checkStateEqual(stateDoc, beforeState);
  }

  TEST_CASE("LayoutStatePromoter - ignores malformed state types", "[uimodel][unit][layout][component]")
  {
    auto malformedSize = LayoutValue{true};

    SECTION("Boolean size")
    {
      malformedSize = LayoutValue{true};
    }

    SECTION("fractional size")
    {
      malformedSize = LayoutValue{320.75};
    }

    SECTION("finite double outside the integer range")
    {
      malformedSize = LayoutValue{0x1p63};
    }

    auto doc = LayoutDocument{};
    doc.root.type = "box";
    doc.root.children.push_back(collapsibleSplitNode("detail-split"));
    auto const baselineHash = componentBaselineHash(doc.root.children[0]);

    auto stateDoc = LayoutComponentStateDocument{.preset = "classic"};
    stateDoc.components["detail-split"] = LayoutComponentStateEntry{
      .type = "collapsibleSplit",
      .stateVersion = kStateEntryVersion,
      .baselineHash = baselineHash,
      .state = {{"size", malformedSize}},
    };

    auto const beforeDocument = doc;
    auto const beforeState = stateDoc;
    CHECK_FALSE(tryPromotePanelSizeDefaults(doc, stateDoc));
    checkDocumentEqual(doc, beforeDocument);
    checkStateEqual(stateDoc, beforeState);

    auto const& unchangedNode = doc.root.children[0];
    CHECK(unchangedNode.props.size() == 4);
    CHECK(unchangedNode.props.at("position").asInt() == 150);
    CHECK(unchangedNode.props.at("initialPositionPercent").asDouble() == 0.25);
    REQUIRE(stateDoc.components.size() == 1);
    auto const& unchangedEntry = stateDoc.components.at("detail-split");
    CHECK(unchangedEntry.baselineHash == baselineHash);
    REQUIRE(unchangedEntry.state.size() == 1);
    CHECK(unchangedEntry.state.at("size").data == malformedSize.data);
  }
} // namespace ao::uimodel::test
