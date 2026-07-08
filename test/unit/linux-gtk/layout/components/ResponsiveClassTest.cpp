// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ContainerTestHelpers.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("ResponsiveClass - updates CSS classes from allocation breakpoints", "[gtk][unit][layout][container]")
  {
    auto fixture = LayoutRuntimeFixture{};
    auto& ctx = fixture.context();
    auto& layoutRuntime = fixture.layoutRuntime();

    auto doc = LayoutDocument{};
    doc.root.type = "responsiveClass";
    doc.root.props["compactMax"] = LayoutValue{static_cast<std::int64_t>(500)};
    doc.root.props["regularMax"] = LayoutValue{static_cast<std::int64_t>(900)};
    doc.root.children.push_back(LayoutNode{.type = "spacer"});

    auto const compPtr = layoutRuntime.build(ctx, doc);
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
} // namespace ao::gtk::layout::test
