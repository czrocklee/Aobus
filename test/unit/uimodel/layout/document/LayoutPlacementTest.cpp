// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutPlacement.h>

#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutNode nodeWithLayout(LayoutValueMap layout)
    {
      return LayoutNode{.type = "box", .layout = std::move(layout)};
    }
  } // namespace

  TEST_CASE("planLayoutPlacement - an absent expansion field is not an authored false",
            "[uimodel][unit][layout][placement]")
  {
    // GTK derives a widget's expansion from its children until something states
    // it, so the two cases are different instructions and cannot collapse.
    auto const absent = planLayoutPlacement(nodeWithLayout({}));

    CHECK_FALSE(absent.optHorizontalExpand);
    CHECK_FALSE(absent.optVerticalExpand);

    auto const authored =
      planLayoutPlacement(nodeWithLayout({{"hexpand", LayoutValue{false}}, {"vexpand", LayoutValue{true}}}));

    REQUIRE(authored.optHorizontalExpand);
    CHECK_FALSE(*authored.optHorizontalExpand);
    REQUIRE(authored.optVerticalExpand);
    CHECK(*authored.optVerticalExpand);
  }

  TEST_CASE("planLayoutPlacement - the authored alignment vocabulary is the document's",
            "[uimodel][unit][layout][placement]")
  {
    CHECK(layoutAlignmentFromString("fill") == LayoutAlignment::Fill);
    CHECK(layoutAlignmentFromString("start") == LayoutAlignment::Start);
    CHECK(layoutAlignmentFromString("end") == LayoutAlignment::End);
    CHECK(layoutAlignmentFromString("center") == LayoutAlignment::Center);

    // A toolkit's own spelling is not the document's, so it names nothing here.
    CHECK_FALSE(layoutAlignmentFromString("stretch"));
    CHECK_FALSE(layoutAlignmentFromString("left"));
    CHECK_FALSE(layoutAlignmentFromString(""));

    auto const placement = planLayoutPlacement(nodeWithLayout(
      {{"halign", LayoutValue{std::string{"center"}}}, {"valign", LayoutValue{std::string{"nonsense"}}}}));

    CHECK(placement.optHorizontalAlignment == LayoutAlignment::Center);
    CHECK_FALSE(placement.optVerticalAlignment);
  }

  TEST_CASE("planLayoutPlacement - a negative size request asks for no minimum", "[uimodel][unit][layout][placement]")
  {
    auto const placement = planLayoutPlacement(nodeWithLayout(
      {{"widthRequest", LayoutValue{std::int64_t{240}}}, {"heightRequest", LayoutValue{std::int64_t{-1}}}}));

    CHECK(placement.optMinWidth == 240.0);
    CHECK_FALSE(placement.optMinHeight);
    CHECK(placement.widthRequestAuthored);
    CHECK(placement.heightRequestAuthored);

    auto const absent = planLayoutPlacement(nodeWithLayout({}));
    CHECK_FALSE(absent.widthRequestAuthored);
    CHECK_FALSE(absent.heightRequestAuthored);
  }

  TEST_CASE("planLayoutPlacement - runtime state may hide an element but never reveals a hidden one",
            "[uimodel][unit][layout][placement]")
  {
    auto const shown = planLayoutPlacement(nodeWithLayout({{"visible", LayoutValue{true}}}));
    CHECK(shown.optAuthoredVisible == true);
    CHECK(isPlacedElementVisible(shown, true));
    CHECK_FALSE(isPlacedElementVisible(shown, false));

    auto const hidden = planLayoutPlacement(nodeWithLayout({{"visible", LayoutValue{false}}}));
    CHECK(hidden.optAuthoredVisible == false);
    CHECK_FALSE(isPlacedElementVisible(hidden, true));
  }

  TEST_CASE("planLayoutPlacement - saying nothing about visibility is not saying visible",
            "[uimodel][unit][layout][placement]")
  {
    // A component that hides itself because it has nothing to show must survive
    // having its common props applied. Collapsing the two states into `true`
    // reveals a volume control with no volume and an undo bar with no undo.
    auto const unauthored = planLayoutPlacement(nodeWithLayout({}));
    CHECK_FALSE(unauthored.optAuthoredVisible);

    // A frontend that has nothing else to consult still shows it.
    CHECK(isPlacedElementVisible(unauthored, true));
  }

  TEST_CASE("isCommonLayoutProp - names exactly the version 1 fields every frontend interprets",
            "[uimodel][unit][layout][placement]")
  {
    for (auto const* const name :
         {"hexpand", "vexpand", "halign", "valign", "widthRequest", "heightRequest", "visible"})
    {
      CHECK(isCommonLayoutProp(name));
    }

    // Styling is per-frontend, so neither shell's styling field is common.
    CHECK_FALSE(isCommonLayoutProp("cssClasses"));
    CHECK_FALSE(isCommonLayoutProp("styleKey"));
    CHECK_FALSE(isCommonLayoutProp("surface"));
  }
} // namespace ao::uimodel::test
