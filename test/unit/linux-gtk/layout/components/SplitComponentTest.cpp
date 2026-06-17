// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "app/linux-gtk/layout/component/container/ContainerRegistry.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "layout/state/LayoutComponentState.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/state/FakeLayoutComponentStateStore.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/paned.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace ao::gtk::layout::test
{
  using ao::gtk::test::makeRuntime;
  using namespace ao::lmdb::test;

  TEST_CASE("SplitComponent success states", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    SECTION("split with 2 children builds Gtk::Paned")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["position"] = LayoutValue{static_cast<std::int64_t>(200)};
      doc.root.props["resizeStart"] = LayoutValue{false};
      doc.root.props["shrinkEnd"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = splitPaned(*compPtr);

      REQUIRE(paned != nullptr);

      int const expectedPos = 200;
      CHECK(paned->get_position() == expectedPos);
      CHECK(paned->get_resize_start_child() == false);
      CHECK(paned->get_shrink_end_child() == true);
    }

    SECTION("split percent position is applied from container allocation")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = splitPaned(*compPtr);

      REQUIRE(paned != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(1200, 400);

      CHECK(paned->get_position() == 300);
    }

    SECTION("split persisted percent overrides layout percent after allocation")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "main-paned";
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentState.components["main-paned"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kLayoutComponentStateEntryVersion,
        .baselineHash = layoutComponentBaselineHash(doc.root),
        .state = {{"positionPercent", LayoutValue{0.6}}},
      };

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = splitPaned(*compPtr);

      REQUIRE(paned != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(1000, 400);

      CHECK(paned->get_position() == 600);
    }

    SECTION("split persisted percent with stale baseline falls back to layout percent")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "main-paned";
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentState.components["main-paned"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kLayoutComponentStateEntryVersion,
        .baselineHash = "stale",
        .state = {{"positionPercent", LayoutValue{0.6}}},
      };

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = splitPaned(*compPtr);

      REQUIRE(paned != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(1000, 400);

      CHECK(paned->get_position() == 250);
    }

    SECTION("split saves user percent and rebuild restores it")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentStateStore = &stateStore;

      auto doc = LayoutDocument{};
      doc.root.id = "main-paned";
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      {
        auto const compPtr = layoutRuntime.build(ctx, doc);
        auto* const paned = splitPaned(*compPtr);

        REQUIRE(paned != nullptr);

        auto allocationHost = AllocationHost{compPtr->widget()};
        allocationHost.allocateChild(1000, 400);
        paned->set_position(400);
      }

      REQUIRE(stateStore.saveCount() == 1);
      REQUIRE(stateStore.document().components.contains("main-paned"));
      CHECK(stateStore.document().components.at("main-paned").state.at("positionPercent").asDouble() == 0.4);

      ctx.componentState = stateStore.document();

      auto const rebuiltPtr = layoutRuntime.build(ctx, doc);
      auto* const rebuiltPaned = splitPaned(*rebuiltPtr);

      REQUIRE(rebuiltPaned != nullptr);

      auto allocationHost = AllocationHost{rebuiltPtr->widget()};
      allocationHost.allocateChild(800, 400);

      CHECK(rebuiltPaned->get_position() == 320);
    }

    SECTION("anonymous split does not persist user percent")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentStateStore = &stateStore;

      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = splitPaned(*compPtr);

      REQUIRE(paned != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(1000, 400);
      paned->set_position(500);
      drainGtkEventsFor(std::chrono::milliseconds{250});

      CHECK(stateStore.saveCount() == 0);
      CHECK(stateStore.document().components.empty());
    }

    SECTION("split defers pending save when context generation advances")
    {
      auto stateStore = FakeLayoutComponentStateStore{};
      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentStateStore = &stateStore;

      auto doc = LayoutDocument{};
      doc.root.id = "main-paned";
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      {
        auto const compPtr = layoutRuntime.build(ctx, doc);
        auto* const paned = splitPaned(*compPtr);
        REQUIRE(paned != nullptr);

        auto allocationHost = AllocationHost{compPtr->widget()};
        allocationHost.allocateChild(1000, 400);
        paned->set_position(400);

        // Simulate a reset/load/save-defaults operation that invalidates old writes.
        ++ctx.componentStateGeneration;
      }

      drainGtkEventsFor(std::chrono::milliseconds{250});
      CHECK(stateStore.saveCount() == 0);
      CHECK(stateStore.document().components.empty());
    }

    SECTION("split clamps persisted percent to [0, 1]")
    {
      auto doc = LayoutDocument{};
      doc.root.id = "main-paned";
      doc.root.type = "split";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};
      doc.root.props["initialPositionPercent"] = LayoutValue{0.25};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      ctx.activePresetId = "classic";
      ctx.componentState = LayoutComponentStateDocument{.preset = "classic"};
      ctx.componentState.components["main-paned"] = LayoutComponentStateEntry{
        .type = "split",
        .stateVersion = kLayoutComponentStateEntryVersion,
        .baselineHash = layoutComponentBaselineHash(doc.root),
        .state = {{"positionPercent", LayoutValue{1.5}}},
      };

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const paned = splitPaned(*compPtr);
      REQUIRE(paned != nullptr);

      auto allocationHost = AllocationHost{compPtr->widget()};
      allocationHost.allocateChild(800, 400);

      // GTK may clamp the paned handle one pixel inside the total allocation.
      CHECK(paned->get_position() >= 790);

      ctx.componentState.components["main-paned"].state["positionPercent"] = LayoutValue{-0.5};

      auto const lowPtr = layoutRuntime.build(ctx, doc);
      auto* const lowPaned = splitPaned(*lowPtr);
      REQUIRE(lowPaned != nullptr);

      auto lowAllocationHost = AllocationHost{lowPtr->widget()};
      lowAllocationHost.allocateChild(800, 400);

      CHECK(lowPaned->get_position() == 0);
    }
  }
} // namespace ao::gtk::layout::test
