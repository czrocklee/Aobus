// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <gtkmm/centerbox.h>

#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("CenterBox - places start, center, and end children", "[gtk][unit][layout][container]")
  {
    auto const vertical = GENERATE(false, true);
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    auto doc = LayoutDocument{};
    doc.root.type = "centerBox";
    doc.root.props["orientation"] = LayoutValue{std::string{vertical ? "vertical" : "horizontal"}};

    auto startChild = LayoutNode{};
    startChild.type = "label";
    startChild.props["text"] = LayoutValue{std::string{"Start child"}};
    startChild.layout["slot"] = LayoutValue{std::string{"start"}};
    doc.root.children.push_back(startChild);

    auto centerChild = LayoutNode{};
    centerChild.type = "label";
    centerChild.props["text"] = LayoutValue{std::string{"Center child"}};
    centerChild.layout["slot"] = LayoutValue{std::string{"center"}};
    doc.root.children.push_back(centerChild);

    auto endChild = LayoutNode{};
    endChild.type = "label";
    endChild.props["text"] = LayoutValue{std::string{"End child"}};
    endChild.layout["slot"] = LayoutValue{std::string{"end"}};
    doc.root.children.push_back(endChild);

    auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
    auto* const cb = dynamic_cast<Gtk::CenterBox*>(&compPtr->widget());

    REQUIRE(cb != nullptr);
    CHECK(cb->get_orientation() == (vertical ? Gtk::Orientation::VERTICAL : Gtk::Orientation::HORIZONTAL));

    auto* const startLabel = dynamic_cast<Gtk::Label*>(cb->get_start_widget());
    auto* const centerLabel = dynamic_cast<Gtk::Label*>(cb->get_center_widget());
    auto* const endLabel = dynamic_cast<Gtk::Label*>(cb->get_end_widget());
    REQUIRE(startLabel != nullptr);
    REQUIRE(centerLabel != nullptr);
    REQUIRE(endLabel != nullptr);
    CHECK(startLabel->get_text() == "Start child");
    CHECK(centerLabel->get_text() == "Center child");
    CHECK(endLabel->get_text() == "End child");
  }
} // namespace ao::gtk::layout::test
