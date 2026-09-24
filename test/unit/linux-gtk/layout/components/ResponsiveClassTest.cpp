// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;
  using ao::gtk::test::AllocationHost;

  TEST_CASE("ResponsiveClass - updates CSS classes from allocation breakpoints",
            "[gtk][unit][layout][container][geometry]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    SECTION("width axis preserves direct compact regular and wide transitions")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "responsiveClass";
      doc.root.props["compactMax"] = LayoutValue{static_cast<std::int64_t>(500)};
      doc.root.props["regularMax"] = LayoutValue{static_cast<std::int64_t>(900)};
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      auto allocationHost = AllocationHost{widget};

      allocationHost.allocateChild(480, 120);
      CHECK(widget.has_css_class("ao-width-compact"));
      CHECK_FALSE(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));

      allocationHost.allocateChild(700, 120);
      CHECK_FALSE(widget.has_css_class("ao-width-compact"));
      CHECK(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));

      allocationHost.allocateChild(1200, 120);
      CHECK_FALSE(widget.has_css_class("ao-width-compact"));
      CHECK_FALSE(widget.has_css_class("ao-width-regular"));
      CHECK(widget.has_css_class("ao-width-wide"));
    }

    SECTION("width axis uses inclusive compact and regular boundaries")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "responsiveClass";
      doc.root.props["compactMax"] = LayoutValue{static_cast<std::int64_t>(500)};
      doc.root.props["regularMax"] = LayoutValue{static_cast<std::int64_t>(900)};
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      auto allocationHost = AllocationHost{widget};

      allocationHost.allocateChild(500, 120);
      CHECK(widget.has_css_class("ao-width-compact"));
      CHECK_FALSE(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));

      allocationHost.allocateChild(501, 120);
      CHECK_FALSE(widget.has_css_class("ao-width-compact"));
      CHECK(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));

      allocationHost.allocateChild(900, 120);
      CHECK_FALSE(widget.has_css_class("ao-width-compact"));
      CHECK(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));

      allocationHost.allocateChild(901, 120);
      CHECK_FALSE(widget.has_css_class("ao-width-compact"));
      CHECK_FALSE(widget.has_css_class("ao-width-regular"));
      CHECK(widget.has_css_class("ao-width-wide"));
    }

    SECTION("height axis classifies height rather than width at the compact boundary")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "responsiveClass";
      doc.root.props["axis"] = LayoutValue{std::string{"height"}};
      doc.root.props["compactMax"] = LayoutValue{static_cast<std::int64_t>(500)};
      doc.root.props["regularMax"] = LayoutValue{static_cast<std::int64_t>(900)};
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      auto allocationHost = AllocationHost{widget};

      allocationHost.allocateChild(1200, 500);
      CHECK(widget.has_css_class("ao-width-compact"));
      CHECK_FALSE(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));

      allocationHost.allocateChild(1200, 501);
      CHECK_FALSE(widget.has_css_class("ao-width-compact"));
      CHECK(widget.has_css_class("ao-width-regular"));
      CHECK_FALSE(widget.has_css_class("ao-width-wide"));
    }

    SECTION("regular maximum below compact maximum normalizes with a custom prefix")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "responsiveClass";
      doc.root.props["compactMax"] = LayoutValue{static_cast<std::int64_t>(500)};
      doc.root.props["regularMax"] = LayoutValue{static_cast<std::int64_t>(300)};
      doc.root.props["classPrefix"] = LayoutValue{std::string{"panel"}};
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      auto allocationHost = AllocationHost{widget};

      allocationHost.allocateChild(500, 120);
      CHECK(widget.has_css_class("panel-compact"));
      CHECK_FALSE(widget.has_css_class("panel-regular"));
      CHECK_FALSE(widget.has_css_class("panel-wide"));

      allocationHost.allocateChild(501, 120);
      CHECK_FALSE(widget.has_css_class("panel-compact"));
      CHECK_FALSE(widget.has_css_class("panel-regular"));
      CHECK(widget.has_css_class("panel-wide"));
    }

    SECTION("empty prefix falls back to the default width classes")
    {
      auto doc = LayoutDocument{};
      doc.root.type = "responsiveClass";
      doc.root.props["classPrefix"] = LayoutValue{std::string{}};
      doc.root.children.push_back(LayoutNode{.type = "spacer"});

      auto const compPtr = layoutRuntime.build(ctx, preparedLayout(doc));
      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      auto allocationHost = AllocationHost{widget};
      allocationHost.allocateChild(480, 120);

      CHECK(widget.has_css_class("ao-width-compact"));
      CHECK_FALSE(widget.has_css_class("-compact"));
    }
  }
} // namespace ao::gtk::layout::test
