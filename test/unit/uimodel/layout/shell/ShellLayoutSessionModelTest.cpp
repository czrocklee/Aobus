// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellLayoutSessionModel.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutNode splitNode(std::string_view id)
    {
      auto node = LayoutNode{};
      node.id = std::string{id};
      node.type = "split";
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      node.props["position"] = LayoutValue{static_cast<std::int64_t>(240)};
      node.children.push_back(LayoutNode{.type = "spacer"});
      node.children.push_back(LayoutNode{.type = "spacer"});
      return node;
    }

    LayoutDocument panelLayoutDocument()
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.children.push_back(splitNode("library-panel"));
      return doc;
    }

    LayoutComponentStateDocument panelRuntimeState(LayoutDocument const& doc)
    {
      auto stateDoc = LayoutComponentStateDocument{.preset = "modern"};
      auto const& split = doc.root.children.front();
      stateDoc.components["library-panel"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kStateEntryVersion,
        .baselineHash = componentBaselineHash(split),
        .state = {{"positionPercent", LayoutValue{0.68}}},
      };
      return stateDoc;
    }
  } // namespace

  TEST_CASE("ShellLayoutSessionModel - manages reusable layout session policy", "[uimodel][unit][layout][shell]")
  {
    SECTION("preset selection accepts supported ids and falls back for unknown non-empty ids")
    {
      static constexpr auto kSupported = std::array<std::string_view, 2>{"classic", "modern"};

      auto const modern = ShellLayoutSessionModel::selectPreset("modern", kSupported);
      CHECK(modern.presetId == "modern");
      CHECK_FALSE(modern.usedFallback);

      auto const empty = ShellLayoutSessionModel::selectPreset("", kSupported);
      CHECK(empty.presetId == "classic");
      CHECK_FALSE(empty.usedFallback);

      auto const unknown = ShellLayoutSessionModel::selectPreset("wide", kSupported);
      CHECK(unknown.presetId == "classic");
      CHECK(unknown.usedFallback);
    }

    SECTION("loaded and editor-saved layouts update the active session identity")
    {
      auto model = ShellLayoutSessionModel{};
      auto initialDoc = panelLayoutDocument();

      model.applyLoadedLayout("modern", initialDoc);
      CHECK(model.snapshot().presetId == "modern");
      CHECK(model.snapshot().layout.root.children.front().id == "library-panel");

      auto savedDoc = LayoutDocument{};
      savedDoc.root.type = "stack";
      model.applyEditorSave("classic", savedDoc);
      CHECK(model.snapshot().presetId == "classic");
      CHECK(model.snapshot().layout.root.type == "stack");
    }

    SECTION("runtime reset uses the default preset before a layout is loaded")
    {
      auto model = ShellLayoutSessionModel{};

      auto reset = model.resetRuntimeLayoutState();

      CHECK(reset.presetId == "classic");
      CHECK(reset.componentState.preset == "classic");
      CHECK(reset.componentState.components.empty());
      CHECK(model.snapshot().presetId == "classic");
    }

    SECTION("runtime reset preserves the active preset and leaves the active layout intact")
    {
      auto model = ShellLayoutSessionModel{};
      auto doc = panelLayoutDocument();
      model.applyLoadedLayout("modern", doc);

      auto reset = model.resetRuntimeLayoutState();

      CHECK(reset.presetId == "modern");
      CHECK(reset.componentState.preset == "modern");
      CHECK(reset.componentState.components.empty());
      CHECK(model.snapshot().layout.root.children.front().id == "library-panel");
    }

    SECTION("panel size promotion folds runtime state into the active layout")
    {
      auto model = ShellLayoutSessionModel{};
      auto doc = panelLayoutDocument();
      auto stateDoc = panelRuntimeState(doc);
      model.applyLoadedLayout("modern", doc);

      auto optPromotion = model.preparePanelSizePromotion(stateDoc);

      REQUIRE(optPromotion);
      CHECK(optPromotion->presetId == "modern");
      CHECK(optPromotion->result.promotedCount == 1);
      CHECK(optPromotion->componentState.components.empty());

      auto const& promotedSplit = optPromotion->layout.root.children.front();
      CHECK(promotedSplit.props.find("position") == promotedSplit.props.end());
      CHECK(promotedSplit.props.at("initialPositionPercent").asDouble() == 0.68);

      model.applyPanelSizePromotion(std::move(*optPromotion));
      CHECK(model.snapshot().layout.root.children.front().props.at("initialPositionPercent").asDouble() == 0.68);
    }

    SECTION("panel size promotion reports no work when runtime state has no promotable entries")
    {
      auto model = ShellLayoutSessionModel{};
      model.applyLoadedLayout("modern", panelLayoutDocument());

      auto stateDoc = LayoutComponentStateDocument{.preset = "modern"};

      CHECK_FALSE(model.preparePanelSizePromotion(stateDoc).has_value());
    }
  }
} // namespace ao::uimodel::test
