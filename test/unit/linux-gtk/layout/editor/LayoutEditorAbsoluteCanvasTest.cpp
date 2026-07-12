// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "../components/ContainerTestHelpers.h"
#include "app/linux-gtk/app/GtkUiDependencies.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutBuildContext.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntimeState.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <utility>

namespace ao::gtk::layout::editor::test
{
  using namespace uimodel;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("absoluteCanvas - builds positioned child containers", "[gtk][unit][layout][editor]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.canvas_test");

    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto runtimeState = LayoutRuntimeState{};
    auto dependencies = GtkUiDependencies{};
    auto ctx = LayoutBuildContext{.registry = registry,
                                  .actionRegistry = actionRegistry,
                                  .runtime = runtime,
                                  .parentWindow = window,
                                  .runtimeState = runtimeState,
                                  .dependencies = dependencies};

    SECTION("absoluteCanvas descriptor is registered as container")
    {
      auto const optDesc = registry.descriptor("absoluteCanvas");

      REQUIRE(optDesc);
      CHECK(uimodel::isContainer(*optDesc));
      CHECK(optDesc->minChildren == 0);
      CHECK(!optDesc->optMaxChildren);
    }

    SECTION("absoluteCanvas with no children builds a component")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "absoluteCanvas";

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      CHECK(compPtr != nullptr);
    }

    SECTION("absoluteCanvas with positioned child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "absoluteCanvas";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.id = "position-spacer";
      child.layout["x"] = LayoutValue{static_cast<std::int64_t>(50)};
      child.layout["y"] = LayoutValue{static_cast<std::int64_t>(100)};
      child.layout["width"] = LayoutValue{static_cast<std::int64_t>(200)};
      child.layout["height"] = LayoutValue{static_cast<std::int64_t>(50)};
      child.layout["zIndex"] = LayoutValue{static_cast<std::int64_t>(2)};
      doc.root.children.push_back(std::move(child));

      auto layoutRuntime = LayoutRuntime{registry};
      auto const compPtr = layoutRuntime.build(ctx, doc);

      CHECK(compPtr != nullptr);
    }
  }

  TEST_CASE("absoluteCanvas - geometry allocates children at configured coordinates", "[gtk][unit][geometry]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.canvas_geometry_test");

    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto runtimeState = LayoutRuntimeState{};
    auto dependencies = GtkUiDependencies{};
    auto ctx = LayoutBuildContext{.registry = registry,
                                  .actionRegistry = actionRegistry,
                                  .runtime = runtime,
                                  .parentWindow = window,
                                  .runtimeState = runtimeState,
                                  .dependencies = dependencies};

    auto doc = LayoutDocument{};
    doc.root.type = "absoluteCanvas";

    auto child = LayoutNode{};
    child.type = "spacer";
    child.id = "position-spacer";
    child.layout["x"] = LayoutValue{static_cast<std::int64_t>(50)};
    child.layout["y"] = LayoutValue{static_cast<std::int64_t>(100)};
    child.layout["width"] = LayoutValue{static_cast<std::int64_t>(200)};
    child.layout["height"] = LayoutValue{static_cast<std::int64_t>(50)};
    doc.root.children.push_back(std::move(child));

    auto layoutRuntime = LayoutRuntime{registry};
    auto const compPtr = layoutRuntime.build(ctx, doc);

    REQUIRE(compPtr != nullptr);

    auto& canvas = compPtr->widget();
    auto const horizontal = ao::gtk::layout::test::measureWidget(canvas, Gtk::Orientation::HORIZONTAL);
    auto const vertical = ao::gtk::layout::test::measureWidget(canvas, Gtk::Orientation::VERTICAL);

    CHECK(horizontal.minimum == 250);
    CHECK(horizontal.natural == 250);
    CHECK(vertical.minimum == 150);
    CHECK(vertical.natural == 150);

    auto allocationHost = ao::gtk::layout::test::AllocationHost{canvas};
    allocationHost.allocateChild(400, 300);

    auto* const allocatedChild = canvas.get_first_child();
    REQUIRE(allocatedChild != nullptr);

    auto const allocation = allocatedChild->get_allocation();
    CHECK(allocation.get_x() == 50);
    CHECK(allocation.get_y() == 100);
    CHECK(allocation.get_width() == 200);
    CHECK(allocation.get_height() == 50);
  }
} // namespace ao::gtk::layout::editor::test
