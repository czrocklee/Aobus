// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("CenterBox component places start, center, and end children", "[gtk][unit][layout][container]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

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
