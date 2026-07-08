// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "app/linux-gtk/app/GtkStyleRuntime.h"
#include "app/linux-gtk/layout/runtime/LayoutHost.h"
#include "layout/document/LayoutDocument.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>

#include <memory>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("LayoutRuntime - builds documents into GTK widget trees", "[gtk][unit][layout][container]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& registry = fixture.components();
    auto& window = fixture.window();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("default layout builds a box root")
    {
      auto const doc = makeDefaultLayout();
      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);

      REQUIRE(rootComponentPtr != nullptr);

      auto& widget = rootComponentPtr->widget();
      CHECK(dynamic_cast<Gtk::Box*>(&widget) != nullptr);
    }

    SECTION("nested layout preserves root orientation")
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

    SECTION("unknown component type produces error label")
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
      auto const templates = builtInTemplates();
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
      auto doc = makeDefaultLayout();
      doc.templates = builtInTemplates();
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

    SECTION("LayoutHost expands the active root to fill the shell")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";
      doc.root.props["orientation"] = LayoutValue{std::string{"vertical"}};

      auto host = LayoutHost{registry};
      host.setLayout(ctx, doc);

      auto* const activeRoot = host.get_first_child();
      REQUIRE(activeRoot != nullptr);
      CHECK(activeRoot->get_hexpand());
      CHECK(activeRoot->get_vexpand());

      auto allocationHost = AllocationHost{host};
      allocationHost.allocateChild(320, 240);

      CHECK(activeRoot->get_width() == 320);
      CHECK(activeRoot->get_height() == 240);
    }

    SECTION("Modern controls bar reserves enough height for padded controls")
    {
      GtkStyleRuntime::instance().initialize();

      auto const doc = makeBuiltInLayout(LayoutPresetId::Modern);
      auto const rootComponentPtr = layoutRuntime.build(ctx, doc);
      REQUIRE(rootComponentPtr != nullptr);

      window.add_css_class("ao-theme-modern");
      window.set_child(rootComponentPtr->widget());

      auto* const controlsBar = ao::gtk::test::findWidgetByClass<Gtk::Widget>(window, "ao-track-controls-bar-modern");
      REQUIRE(controlsBar != nullptr);
      auto* const quickFilterEntry = ao::gtk::test::findWidgetByClass<Gtk::Entry>(window, "ao-quick-filter-entry");
      REQUIRE(quickFilterEntry != nullptr);
      auto* const presentationButton = ao::gtk::test::findWidget<Gtk::MenuButton>(window);
      REQUIRE(presentationButton != nullptr);

      auto const verticalMeasure = measureWidget(*controlsBar, Gtk::Orientation::VERTICAL);
      CHECK(verticalMeasure.minimum >= 58);
      CHECK(verticalMeasure.natural >= 58);

      controlsBar->size_allocate(Gtk::Allocation{0, 0, 2036, 58}, -1);
      CHECK(quickFilterEntry->get_height() >= 36);
      CHECK(presentationButton->get_height() >= 24);

      window.unset_child();
      window.remove_css_class("ao-theme-modern");
    }
  }
} // namespace ao::gtk::layout::test
