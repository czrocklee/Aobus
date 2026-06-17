// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "app/linux-gtk/layout/component/container/ContainerRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/window.h>

#include <cstdint>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using ao::gtk::test::makeRuntime;
  using namespace ao::lmdb::test;

  TEST_CASE("ResponsiveClass component", "[layout][unit][containers]")
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

    auto doc = LayoutDocument{};
    doc.root.type = "responsiveClass";
    doc.root.props["compactMax"] = LayoutValue{static_cast<std::int64_t>(500)};
    doc.root.props["regularMax"] = LayoutValue{static_cast<std::int64_t>(900)};
    doc.root.children.push_back(LayoutNode{.type = "spacer"});

    auto const compPtr = layoutRuntime.build(ctx, doc);
    REQUIRE(compPtr != nullptr);

    auto& widget = compPtr->widget();
    auto allocationHost = AllocationHost{widget};

    allocationHost.allocateChild(480, 120);
    CHECK(widget.has_css_class("ao-width-compact"));
    CHECK_FALSE(widget.has_css_class("ao-width-regular"));
    CHECK_FALSE(widget.has_css_class("ao-width-wide"));

    allocationHost.allocateChild(700, 120);
    CHECK_FALSE(widget.has_css_class("ao-width-compact"));
    CHECK(widget.has_css_class("ao-width-regular"));
    CHECK_FALSE(widget.has_css_class("ao-width-wide"));

    allocationHost.allocateChild(1200, 120);
    CHECK_FALSE(widget.has_css_class("ao-width-compact"));
    CHECK_FALSE(widget.has_css_class("ao-width-regular"));
    CHECK(widget.has_css_class("ao-width-wide"));
  }
} // namespace ao::gtk::layout::test
