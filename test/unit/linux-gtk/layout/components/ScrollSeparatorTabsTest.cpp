// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/stackpage.h>

#include <cstdint>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("ScrollSeparatorTabs - scroll maps explicit and default policies",
            "[gtk][unit][layout-component][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

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

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
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

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const sw = dynamic_cast<Gtk::ScrolledWindow*>(&compPtr->widget());

      REQUIRE(sw != nullptr);

      auto hpolicy = Gtk::PolicyType::NEVER;
      auto vpolicy = Gtk::PolicyType::NEVER;
      sw->get_policy(hpolicy, vpolicy);

      CHECK(hpolicy == Gtk::PolicyType::AUTOMATIC);
      CHECK(vpolicy == Gtk::PolicyType::AUTOMATIC);
    }
  }

  TEST_CASE("ScrollSeparatorTabs - separator applies authored orientation", "[gtk][unit][layout-component]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    auto doc = LayoutDocument{};
    doc.root.type = "separator";
    doc.root.props["orientation"] = LayoutValue{std::string{"vertical"}};

    auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
    auto* const sep = dynamic_cast<Gtk::Separator*>(&compPtr->widget());

    REQUIRE(sep != nullptr);
    CHECK(sep->get_orientation() == Gtk::Orientation::VERTICAL);
  }

  TEST_CASE("ScrollSeparatorTabs - tabs preserve authored identity title and order", "[gtk][unit][layout-component]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

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

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());

      REQUIRE(box != nullptr);

      auto* const firstChild = box->get_first_child();
      REQUIRE(firstChild != nullptr);

      auto* const stackWidget = firstChild->get_next_sibling();
      REQUIRE(stackWidget != nullptr);

      auto* const stack = dynamic_cast<Gtk::Stack*>(stackWidget);
      REQUIRE(stack != nullptr);

      auto* const firstStackChild = stack->get_first_child();
      REQUIRE(firstStackChild != nullptr);
      auto* const secondStackChild = firstStackChild->get_next_sibling();
      REQUIRE(secondStackChild != nullptr);
      CHECK(secondStackChild->get_next_sibling() == nullptr);
      CHECK(stack->get_child_by_name("tab1") == firstStackChild);
      CHECK(stack->get_child_by_name("tab2") == secondStackChild);

      auto const firstPagePtr = stack->get_page(*firstStackChild);
      REQUIRE(firstPagePtr);
      CHECK(firstPagePtr->get_name() == "tab1");
      CHECK(firstPagePtr->get_title() == "First Tab");

      auto const secondPagePtr = stack->get_page(*secondStackChild);
      REQUIRE(secondPagePtr);
      CHECK(secondPagePtr->get_name() == "tab2");
      CHECK(secondPagePtr->get_title() == "Second Tab");
    }

    SECTION("tabs child without id uses type as tab name")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";

      auto c1 = LayoutNode{};
      c1.type = "spacer";
      c1.layout["title"] = LayoutValue{std::string{"Spacer Tab"}};
      doc.root.children.push_back(c1);

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));

      REQUIRE(compPtr != nullptr);

      auto* const box = dynamic_cast<Gtk::Box*>(&compPtr->widget());
      REQUIRE(box != nullptr);
      auto* const switcher = box->get_first_child();
      REQUIRE(switcher != nullptr);
      auto* const stack = dynamic_cast<Gtk::Stack*>(switcher->get_next_sibling());
      REQUIRE(stack != nullptr);
      auto* const stackChild = stack->get_first_child();
      REQUIRE(stackChild != nullptr);
      CHECK(stackChild->get_next_sibling() == nullptr);
      CHECK(stack->get_child_by_name("spacer") == stackChild);

      auto const pagePtr = stack->get_page(*stackChild);
      REQUIRE(pagePtr);
      CHECK(pagePtr->get_name() == "spacer");
      CHECK(pagePtr->get_title() == "Spacer Tab");
    }
  }
} // namespace ao::gtk::layout::test
