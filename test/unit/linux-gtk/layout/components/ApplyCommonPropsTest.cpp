// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("applyCommonProps applies GTK widget layout properties", "[gtk][unit][layout][containers]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("hexpand/vexpand applied to child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["hexpand"] = LayoutValue{true};
      child.layout["vexpand"] = LayoutValue{false};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->get_hexpand() == true);
      CHECK(spacer->get_vexpand() == false);
    }

    SECTION("halign/valign applied to child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["halign"] = LayoutValue{std::string{"center"}};
      child.layout["valign"] = LayoutValue{std::string{"end"}};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->get_halign() == Gtk::Align::CENTER);
      CHECK(spacer->get_valign() == Gtk::Align::END);
    }

    SECTION("visible=false hides child")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["visible"] = LayoutValue{false};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->get_visible() == false);
    }

    SECTION("cssClasses applied from layout")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["cssClasses"] = LayoutValue{std::vector<std::string>{"my-class", "another"}};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);
      CHECK(spacer->has_css_class("my-class"));
      CHECK(spacer->has_css_class("another"));
    }

    SECTION("widthRequest/heightRequest set size request")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "box";

      auto child = LayoutNode{};
      child.type = "spacer";
      child.layout["widthRequest"] = LayoutValue{static_cast<std::int64_t>(200)};
      child.layout["heightRequest"] = LayoutValue{static_cast<std::int64_t>(100)};
      doc.root.children.push_back(child);

      auto const compPtr = layoutRuntime.build(ctx, doc);
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const spacer = box->get_first_child();
      REQUIRE(spacer != nullptr);

      std::int32_t width = -1;
      std::int32_t height = -1;
      spacer->get_size_request(width, height);

      int const expectedW = 200;
      int const expectedH = 100;
      CHECK(width == expectedW);
      CHECK(height == expectedH);
    }
  }
} // namespace ao::gtk::layout::test
