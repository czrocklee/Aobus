// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/LayoutHost.h"

#include "app/linux-gtk/layout/component/container/ContainerRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("LayoutHost - rebuilds widget trees after layout updates", "[gtk][unit][layout][container]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.layout_test");

    auto const tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    registerContainerComponents(registry);

    auto window = Gtk::Window{};
    auto const actionRegistry = ActionRegistry{};
    auto ctx =
      LayoutContext{.registry = registry, .actionRegistry = actionRegistry, .runtime = runtime, .parentWindow = window};

    auto host = LayoutHost{registry};

    SECTION("Initial layout is empty before setLayout")
    {
      CHECK(host.get_first_child() == nullptr);
    }

    SECTION("setLayout with default document populates widget")
    {
      host.setLayout(ctx, createDefaultLayout());

      auto* const child = host.get_first_child();

      REQUIRE(child != nullptr);
      CHECK(dynamic_cast<Gtk::Widget*>(child) != nullptr);
    }

    SECTION("setLayout replaces previous layout")
    {
      host.setLayout(ctx, createDefaultLayout());

      auto* const first = host.get_first_child();
      CHECK(first != nullptr);

      auto newDoc = LayoutDocument{};
      newDoc.root.type = "spacer";
      host.setLayout(ctx, newDoc);

      auto* const second = host.get_first_child();
      CHECK(second != nullptr);
      CHECK(second != first);
    }

    SECTION("setLayout renders registered semantic components")
    {
      auto registry2 = ComponentRegistry{};
      LayoutRuntime::registerStandardComponents(registry2);

      auto window2 = Gtk::Window{};
      auto const tempDir2 = ao::test::TempDir{};
      auto runtime2 = makeRuntime(tempDir2);
      auto const actionRegistry2 = ActionRegistry{};
      auto ctx2 = LayoutContext{
        .registry = registry2, .actionRegistry = actionRegistry2, .runtime = runtime2, .parentWindow = window2};

      auto doc = LayoutDocument{};
      doc.root.type = "status.messageLabel";

      auto host2 = LayoutHost{registry2};
      host2.setLayout(ctx2, doc);

      auto* const label = dynamic_cast<Gtk::Label*>(host2.get_first_child());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Aobus Ready");
    }
  }
} // namespace ao::gtk::layout::test
