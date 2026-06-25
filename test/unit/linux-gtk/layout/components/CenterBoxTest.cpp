// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/container/ContainerRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>
#include <gtkmm/window.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("CenterBox component", "[layout][unit][containers]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto layoutRuntime = LayoutRuntime{registry};

    auto doc = LayoutDocument{};
    doc.root.type = "centerBox";
    doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};

    auto c1 = LayoutNode{};
    c1.type = "spacer";
    c1.layout["slot"] = LayoutValue{std::string{"start"}};
    doc.root.children.push_back(c1);

    auto c2 = LayoutNode{};
    c2.type = "spacer";
    c2.layout["slot"] = LayoutValue{std::string{"center"}};
    doc.root.children.push_back(c2);

    auto c3 = LayoutNode{};
    c3.type = "spacer";
    c3.layout["slot"] = LayoutValue{std::string{"end"}};
    doc.root.children.push_back(c3);

    auto const compPtr = layoutRuntime.build(ctx, doc);
    auto* const cb = dynamic_cast<Gtk::CenterBox*>(&compPtr->widget());

    REQUIRE(cb != nullptr);
    CHECK(cb->get_orientation() == Gtk::Orientation::HORIZONTAL);
    CHECK(cb->get_start_widget() != nullptr);
    CHECK(cb->get_center_widget() != nullptr);
    CHECK(cb->get_end_widget() != nullptr);
  }
} // namespace ao::gtk::layout::test
