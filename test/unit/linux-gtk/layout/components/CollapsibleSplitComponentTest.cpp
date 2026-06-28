// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "test/unit/linux-gtk/layout/state/FakeLayoutComponentStateStore.h"
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/revealer.h>

#include <cstdint>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::emitClicked;

  TEST_CASE("CollapsibleSplitComponent applies reveal sizing and persists panel state", "[gtk][unit][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("collapsibleSplit wraps the collapsible child in a revealer sizing pane")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(420)};
      doc.root.props["revealed"] = LayoutValue{false};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::HORIZONTAL);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);
      CHECK(workspace->get_hexpand() == true);

      auto* const gutterBox = dynamic_cast<Gtk::Box*>(workspace->get_next_sibling());
      REQUIRE(gutterBox != nullptr);
      CHECK(gutterBox->has_css_class("ao-detail-resize-grip"));

      auto* const handleWidget = gutterBox->get_first_child();
      REQUIRE(handleWidget != nullptr);
      auto* const handleButton = dynamic_cast<Gtk::Button*>(handleWidget);
      REQUIRE(handleButton != nullptr);
      CHECK(handleButton->get_valign() == Gtk::Align::CENTER);
      CHECK_FALSE(handleButton->get_vexpand());

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);
      CHECK(revealer->get_reveal_child() == false);
      CHECK(revealer->get_transition_type() == Gtk::RevealerTransitionType::SLIDE_LEFT);

      emitClicked(*handleButton);
      CHECK(revealer->get_reveal_child() == true);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedWidth = 420;
      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == expectedWidth);
      CHECK(horizontalMeasure.natural == expectedWidth);
    }

    SECTION("collapsibleSplit pane size is not expanded by child minimum width")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(120)};

      auto workspaceNode = LayoutNode{.type = "spacer"};
      auto detailNode = LayoutNode{.type = "spacer"};
      detailNode.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(900)};
      doc.root.children.push_back(workspaceNode);
      doc.root.children.push_back(detailNode);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedWidth = 120;
      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == expectedWidth);
      CHECK(horizontalMeasure.natural == expectedWidth);
      CHECK(paneSizer->get_overflow() == Gtk::Overflow::HIDDEN);
    }

    SECTION("collapsibleSplit start side places the revealer before the handle")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"vertical"}};
      doc.root.props["collapseSide"] = LayoutValue{std::string{"start"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::VERTICAL);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(box->get_first_child());
      REQUIRE(revealer != nullptr);
      CHECK(revealer->get_reveal_child() == true);
      CHECK(revealer->get_transition_type() == Gtk::RevealerTransitionType::SLIDE_DOWN);

      auto* const gutterBox = dynamic_cast<Gtk::Box*>(revealer->get_next_sibling());
      REQUIRE(gutterBox != nullptr);
      CHECK(gutterBox->has_css_class("ao-detail-resize-grip"));

      auto* const handleWidget = gutterBox->get_first_child();
      REQUIRE(handleWidget != nullptr);
      auto* const handleButton = dynamic_cast<Gtk::Button*>(handleWidget);
      REQUIRE(handleButton != nullptr);

      emitClicked(*handleButton);
      CHECK(revealer->get_reveal_child() == false);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedHeight = 180;
      auto const verticalMeasure = measureWidget(*paneSizer, Gtk::Orientation::VERTICAL);
      CHECK(verticalMeasure.minimum == expectedHeight);
      CHECK(verticalMeasure.natural == expectedHeight);
    }

    SECTION("collapsibleSplit invalid position falls back to default detail size")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(-1)};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      int const expectedDefaultWidth = 0;
      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == expectedDefaultWidth);
      CHECK(horizontalMeasure.natural == expectedDefaultWidth);
    }

    SECTION("collapsibleSplit percent size follows later container allocations before manual resize")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.3};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const workspace = box->get_first_child();
      REQUIRE(workspace != nullptr);

      auto* const gutterBox = workspace->get_next_sibling();
      REQUIRE(gutterBox != nullptr);

      auto* const revealer = dynamic_cast<Gtk::Revealer*>(gutterBox->get_next_sibling());
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};

      allocationHost.allocateChild(834, 300);
      auto firstMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(firstMeasure.minimum == 250);
      CHECK(firstMeasure.natural == 250);

      allocationHost.allocateChild(2000, 300);
      auto secondMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(secondMeasure.minimum == 600);
      CHECK(secondMeasure.natural == 600);
    }

    SECTION("collapsibleSplit persisted size overrides layout defaults and clamps to narrow allocation")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(420)};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.3};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentState.components["detail-split"] = LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = kLayoutComponentStateEntryVersion,
        .baselineHash = layoutComponentBaselineHash(doc.root),
        .state = {{"size", LayoutValue{static_cast<std::int64_t>(900)}}, {"revealed", LayoutValue{true}}},
      };

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);

      REQUIRE(box != nullptr);

      auto* const revealer = endSideCollapsibleRevealer(*box);
      REQUIRE(revealer != nullptr);

      auto* const paneSizer = revealer->get_child();
      REQUIRE(paneSizer != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(300, 200);

      auto const horizontalMeasure = measureWidget(*paneSizer, Gtk::Orientation::HORIZONTAL);
      CHECK(horizontalMeasure.minimum == 238);
      CHECK(horizontalMeasure.natural == 238);
    }

    SECTION("collapsibleSplit persisted revealed restores only with matching baseline")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentState.components["detail-split"] = LayoutComponentStateEntry{
        .type = "collapsibleSplit",
        .stateVersion = kLayoutComponentStateEntryVersion,
        .baselineHash = layoutComponentBaselineHash(doc.root),
        .state = {{"size", LayoutValue{static_cast<std::int64_t>(180)}}, {"revealed", LayoutValue{false}}},
      };

      auto const restoredPtr = layoutRuntime.build(ctx, doc);
      auto* const restoredBox = collapsibleSplitBox(*restoredPtr);
      REQUIRE(restoredBox != nullptr);

      auto* const restoredRevealer = endSideCollapsibleRevealer(*restoredBox);
      REQUIRE(restoredRevealer != nullptr);
      CHECK(restoredRevealer->get_reveal_child() == false);

      ctx.componentState.components["detail-split"].baselineHash = "stale";

      auto const fallbackPtr = layoutRuntime.build(ctx, doc);
      auto* const fallbackBox = collapsibleSplitBox(*fallbackPtr);
      REQUIRE(fallbackBox != nullptr);

      auto* const fallbackRevealer = endSideCollapsibleRevealer(*fallbackBox);
      REQUIRE(fallbackRevealer != nullptr);
      CHECK(fallbackRevealer->get_reveal_child() == true);
    }

    SECTION("collapsibleSplit toggle persists revealed state and current size")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentStateStore = &stateStore;

      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);
      REQUIRE(box != nullptr);

      auto* const handleButton = endSideCollapsibleToggle(*box);
      REQUIRE(handleButton != nullptr);

      emitClicked(*handleButton);

      CHECK(stateStore.saveCount() == 1);
      REQUIRE(stateStore.document().components.contains("detail-split"));
      auto const& entry = stateStore.document().components.at("detail-split");
      CHECK(entry.type == "collapsibleSplit");
      CHECK(entry.baselineHash == layoutComponentBaselineHash(doc.root));
      CHECK(entry.state.at("revealed").asBool(true) == false);
      CHECK(entry.state.at("size").asInt() == 180);
    }

    SECTION("collapsibleSplit edit mode toggle does not persist runtime state")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentStateStore = &stateStore;
      ctx.editMode = true;

      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = collapsibleSplitBox(*compPtr);
      REQUIRE(box != nullptr);

      auto* const handleButton = endSideCollapsibleToggle(*box);
      REQUIRE(handleButton != nullptr);

      emitClicked(*handleButton);

      CHECK(stateStore.saveCount() == 0);
      CHECK(stateStore.document().components.empty());
    }

    SECTION("collapsibleSplit ignores state writes after context generation advances")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentStateStore = &stateStore;

      auto doc = LayoutDocument{};
      doc.root.id = "detail-split";
      doc.root.type = "collapsibleSplit";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(180)};
      doc.root.props["revealed"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      {
        auto const compPtr = layoutRuntime.build(ctx, doc);
        auto* const box = collapsibleSplitBox(*compPtr);
        REQUIRE(box != nullptr);

        auto* const handleButton = endSideCollapsibleToggle(*box);
        REQUIRE(handleButton != nullptr);

        ++ctx.componentStateGeneration;
        emitClicked(*handleButton);
      }

      CHECK(stateStore.saveCount() == 0);
      CHECK(stateStore.document().components.empty());
    }
  }
} // namespace ao::gtk::layout::test
