// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/component/container/ContainerRegistry.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutHost.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include <memory>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;
  using ao::gtk::test::makeRuntime;
  using namespace ao::lmdb::test;

  TEST_CASE("LayoutRuntime building", "[layout][unit][containers]")
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

    SECTION("Build default layout")
    {
      auto const doc = createDefaultLayout();
      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      CHECK(dynamic_cast<Gtk::Box*>(&widget) != nullptr);
    }

    SECTION("Build nested layout")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.props["orientation"] = LayoutValue{std::string{"horizontal"}};

      auto child1 = LayoutNode{};
      child1.type = "spacer";
      doc.root.children.push_back(child1);

      auto child2 = LayoutNode{};
      child2.type = "box";
      child2.props["orientation"] = LayoutValue{std::string{"vertical"}};
      doc.root.children.push_back(child2);

      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      auto* const box = dynamic_cast<Gtk::Box*>(&widget);
      REQUIRE(box != nullptr);
      CHECK(box->get_orientation() == Gtk::Orientation::HORIZONTAL);
    }

    SECTION("Unknown component type produces error label")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "nonexistent.component";

      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      auto* const label = dynamic_cast<Gtk::Label*>(&widget);
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("[Layout Error]") != std::string::npos);
      CHECK(label->get_label().find("nonexistent.component") != std::string::npos);
    }

    SECTION("Box component forwards cssClasses to widget")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.layout["cssClasses"] = LayoutValue{std::string{"ao-test-class"}};

      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);
      auto* const box = dynamic_cast<Gtk::Box*>(&rootComponentPtr->widget());
      REQUIRE(box != nullptr);
      CHECK(box->has_css_class("ao-test-class"));
    }

    SECTION("Playback bar groups carry ao-grouping-region (direct template)")
    {
      auto const templates = getBuiltInTemplates();
      auto const& barTemplate = templates.at("playback.defaultBar");
      auto const barCompPtr = ctx.registry.create(ctx, barTemplate);

      REQUIRE(barCompPtr != nullptr);
      auto* const barBox = dynamic_cast<Gtk::Box*>(&barCompPtr->widget());
      REQUIRE(barBox != nullptr);

      auto* const leftChild = barBox->get_first_child();
      REQUIRE(leftChild != nullptr);
      CHECK(leftChild->has_css_class("ao-grouping-region"));

      auto* const rightChild = leftChild->get_next_sibling()->get_next_sibling();
      REQUIRE(rightChild != nullptr);
      CHECK(rightChild->has_css_class("ao-grouping-region"));
    }

    SECTION("Playback bar groups carry ao-grouping-region (via template expansion)")
    {
      // This exercises the same path as the real app: default layout with
      // template reference node, expanded through LayoutRuntime::build().
      auto doc = createDefaultLayout();
      doc.templates = getBuiltInTemplates();
      auto const fullLayoutPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(fullLayoutPtr != nullptr);
      // Find the playback row child within the root box.
      auto* const rootBox = dynamic_cast<Gtk::Box*>(&fullLayoutPtr->widget());
      REQUIRE(rootBox != nullptr);

      // Child order: 0=menuBar, 1=playback-bar (expanded template), 2=split, 3=status bar
      auto* const playbackBar = rootBox->get_first_child()->get_next_sibling();
      REQUIRE(playbackBar != nullptr);
      auto* const barBox = dynamic_cast<Gtk::Box*>(playbackBar);
      REQUIRE(barBox != nullptr);

      auto* const leftChild = barBox->get_first_child();
      REQUIRE(leftChild != nullptr);
      CHECK(leftChild->has_css_class("ao-grouping-region"));

      auto* const rightChild = leftChild->get_next_sibling()->get_next_sibling();
      REQUIRE(rightChild != nullptr);
      CHECK(rightChild->has_css_class("ao-grouping-region"));
    }
  }
} // namespace ao::gtk::layout::test
