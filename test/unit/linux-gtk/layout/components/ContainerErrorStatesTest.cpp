// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/runtime/ILayoutComponent.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("Container components render validation error states", "[gtk][unit][layout][containers]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    auto const checkError = [](ILayoutComponent& compPtr, std::string const& expectedFragment)
    {
      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr.widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_label().find("[Layout Error]") != std::string::npos);
      CHECK(label->get_label().find(expectedFragment) != std::string::npos);
    };

    SECTION("split with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "2 children");
    }

    SECTION("split with 1 child returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "2 children");
    }

    SECTION("split with 3 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "split";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "2 children");
    }

    SECTION("scroll with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "1 child");
    }

    SECTION("scroll with 2 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "scroll";
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      doc.root.children.push_back(LayoutNode{.type = "spacer"});
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "1 child");
    }

    SECTION("tabs with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "tabs";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "at least 1 child");
    }

    SECTION("responsiveClass with 0 children returns error")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "responsiveClass";
      auto const compPtr = layoutRuntime.build(ctx, doc);
      checkError(*compPtr, "1 child");
    }
  }
} // namespace ao::gtk::layout::test
