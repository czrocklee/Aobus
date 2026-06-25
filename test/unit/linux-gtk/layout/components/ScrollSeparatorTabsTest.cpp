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
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using ao::gtk::test::makeRuntime;

  TEST_CASE("Scroll, separator, and tabs components", "[layout][unit][containers][geometry]")
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

    SECTION("scroll with 1 child builds Gtk::ScrolledWindow")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.props["hscrollPolicy"] = LayoutValue{std::string{"never"}};
      doc.root.props["vscrollPolicy"] = LayoutValue{std::string{"always"}};
      doc.root.props["minContentWidth"] = LayoutValue{static_cast<std::int64_t>(400)};
      doc.root.props["minContentHeight"] = LayoutValue{static_cast<std::int64_t>(300)};
      doc.root.props["propagateNaturalWidth"] = LayoutValue{true};
      doc.root.props["propagateNaturalHeight"] = LayoutValue{true};

      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&compPtr->widget());

      REQUIRE(sw != nullptr);

      auto hpolicy = Gtk::PolicyType::NEVER;
      auto vpolicy = Gtk::PolicyType::NEVER;
      sw->get_policy(hpolicy, vpolicy);

      CHECK(hpolicy == Gtk::PolicyType::NEVER);
      CHECK(vpolicy == Gtk::PolicyType::ALWAYS);

      int const expectedW = 400;
      int const expectedH = 300;
      CHECK(sw->get_min_content_width() == expectedW);
      CHECK(sw->get_min_content_height() == expectedH);
      CHECK(sw->get_propagate_natural_width() == true);
      CHECK(sw->get_propagate_natural_height() == true);
    }

    SECTION("scroll with policy defaults uses automatic")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&compPtr->widget());

      REQUIRE(sw != nullptr);

      auto hpolicy = Gtk::PolicyType::NEVER;
      auto vpolicy = Gtk::PolicyType::NEVER;
      sw->get_policy(hpolicy, vpolicy);

      CHECK(hpolicy == Gtk::PolicyType::AUTOMATIC);
      CHECK(vpolicy == Gtk::PolicyType::AUTOMATIC);
    }

    SECTION("separator builds Gtk::Separator")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "separator";
      doc.root.props["orientation"] = LayoutValue{std::string{"vertical"}};

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const sep = dynamic_cast<Gtk::Separator*>(&compPtr->widget());

      REQUIRE(sep != nullptr);
      CHECK(sep->get_orientation() == Gtk::Orientation::VERTICAL);
    }

    SECTION("tabs with children builds Gtk::Stack")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.id = "tab1";
      c1.layout["title"] = LayoutValue{std::string{"First Tab"}};
      doc.root.children.push_back(c1);

      auto c2 = LayoutNode{};
      c2.type = "spacer";
      c2.id = "tab2";
      c2.layout["title"] = LayoutValue{std::string{"Second Tab"}};
      doc.root.children.push_back(c2);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const firstChild = box->get_first_child();
      REQUIRE(firstChild != nullptr);

      auto* const stackWidget = firstChild->get_next_sibling();
      REQUIRE(stackWidget != nullptr);

      auto* const stack = dynamic_cast<Gtk::Stack*>(stackWidget);
      REQUIRE(stack != nullptr);

      auto* const stackChild = stack->get_first_child();
      CHECK(stackChild != nullptr);
    }

    SECTION("tabs child without id uses type as tab name")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.layout["title"] = LayoutValue{std::string{"Spacer Tab"}};
      doc.root.children.push_back(c1);

      auto const compPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);
    }
  }
} // namespace ao::gtk::layout::test
