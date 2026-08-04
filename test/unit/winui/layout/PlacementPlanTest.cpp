// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/PlacementPlan.h>

#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::winui::test
{
  using uimodel::LayoutNode;
  using uimodel::LayoutValue;
  using uimodel::LayoutValueMap;

  namespace
  {
    LayoutNode nodeWithLayout(LayoutValueMap layout)
    {
      return LayoutNode{.type = "box", .layout = std::move(layout)};
    }
  } // namespace

  TEST_CASE("planPlacement - expansion becomes a parent-allocated star slot", "[winui][unit][layout]")
  {
    auto const authored =
      planPlacement(nodeWithLayout({{"hexpand", LayoutValue{true}}, {"vexpand", LayoutValue{false}}}));

    CHECK(authored.horizontalSlot == SlotSizing::Star);
    CHECK(authored.verticalSlot == SlotSizing::Auto);

    auto const unauthored = planPlacement(nodeWithLayout({}));
    CHECK(unauthored.horizontalSlot == SlotSizing::Auto);
    CHECK(unauthored.verticalSlot == SlotSizing::Auto);
  }

  TEST_CASE("planPlacement - alignment maps the version 1 vocabulary onto both axes", "[winui][unit][layout]")
  {
    auto const endCenter = planPlacement(
      nodeWithLayout({{"halign", LayoutValue{std::string{"end"}}}, {"valign", LayoutValue{std::string{"center"}}}}));
    CHECK(endCenter.optHorizontalAlignment == HorizontalAlignment::Right);
    CHECK(endCenter.optVerticalAlignment == VerticalAlignment::Center);

    auto const fillStart = planPlacement(
      nodeWithLayout({{"halign", LayoutValue{std::string{"fill"}}}, {"valign", LayoutValue{std::string{"start"}}}}));
    CHECK(fillStart.optHorizontalAlignment == HorizontalAlignment::Stretch);
    CHECK(fillStart.optVerticalAlignment == VerticalAlignment::Top);

    auto const startEnd = planPlacement(
      nodeWithLayout({{"halign", LayoutValue{std::string{"start"}}}, {"valign", LayoutValue{std::string{"end"}}}}));
    CHECK(startEnd.optHorizontalAlignment == HorizontalAlignment::Left);
    CHECK(startEnd.optVerticalAlignment == VerticalAlignment::Bottom);
  }

  TEST_CASE("planPlacement - an unauthored field leaves the style default in effect", "[winui][unit][layout]")
  {
    auto const plan = planPlacement(nodeWithLayout({{"halign", LayoutValue{std::string{"start"}}}}));

    CHECK(plan.optHorizontalAlignment);
    CHECK_FALSE(plan.optVerticalAlignment);
    CHECK_FALSE(plan.optMinWidth);
    CHECK_FALSE(plan.optMinHeight);
  }

  TEST_CASE("planPlacement - a size request becomes a minimum and a negative request clears it",
            "[winui][unit][layout]")
  {
    auto const requested = planPlacement(nodeWithLayout(
      {{"widthRequest", LayoutValue{std::int64_t{320}}}, {"heightRequest", LayoutValue{std::int64_t{48}}}}));

    CHECK(requested.optMinWidth == 320.0);
    CHECK(requested.optMinHeight == 48.0);

    auto const unset = planPlacement(nodeWithLayout({{"widthRequest", LayoutValue{std::int64_t{-1}}}}));
    CHECK(unset.optMinWidth == 0.0);
  }

  TEST_CASE("planPlacement - an authored hide is carried into the plan", "[winui][unit][layout]")
  {
    // How an authored hide combines with runtime state is a shared rule, held
    // by uimodel::isPlacedElementVisible. All the plan owes is carrying it.
    CHECK_FALSE(planPlacement(nodeWithLayout({{"visible", LayoutValue{false}}})).authoredVisible);
    CHECK(planPlacement(nodeWithLayout({{"visible", LayoutValue{true}}})).authoredVisible);
    CHECK(planPlacement(nodeWithLayout({})).authoredVisible);
  }
} // namespace ao::winui::test
