// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ElementKind.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <utility>

namespace ao::winui::test
{
  TEST_CASE("ElementKind - a style applies to its target kind and everything below it", "[winui][unit][layout]")
  {
    CHECK(isElementKindDerivedFrom(ElementKind::Button, ElementKind::Button));
    CHECK(isElementKindDerivedFrom(ElementKind::Button, ElementKind::ButtonBase));
    CHECK(isElementKindDerivedFrom(ElementKind::Button, ElementKind::ContentControl));
    CHECK(isElementKindDerivedFrom(ElementKind::Button, ElementKind::Control));
    CHECK(isElementKindDerivedFrom(ElementKind::Button, ElementKind::FrameworkElement));

    CHECK(isElementKindDerivedFrom(ElementKind::Grid, ElementKind::Panel));
    CHECK(isElementKindDerivedFrom(ElementKind::ListView, ElementKind::ItemsControl));
  }

  TEST_CASE("ElementKind - unrelated branches never accept each other's styles", "[winui][unit][layout]")
  {
    CHECK_FALSE(isElementKindDerivedFrom(ElementKind::Button, ElementKind::ItemsControl));
    CHECK_FALSE(isElementKindDerivedFrom(ElementKind::Grid, ElementKind::Border));
    CHECK_FALSE(isElementKindDerivedFrom(ElementKind::TextBlock, ElementKind::Control));
    // A base never satisfies a more specific target.
    CHECK_FALSE(isElementKindDerivedFrom(ElementKind::Control, ElementKind::Button));
  }

  TEST_CASE("ElementKind - target type names round trip", "[winui][unit][layout]")
  {
    constexpr auto kExpected = std::to_array<std::pair<ElementKind, std::string_view>>({
      {ElementKind::FrameworkElement, "FrameworkElement"},
      {ElementKind::Panel, "Panel"},
      {ElementKind::Grid, "Grid"},
      {ElementKind::Border, "Border"},
      {ElementKind::TextBlock, "TextBlock"},
      {ElementKind::Control, "Control"},
      {ElementKind::ContentControl, "ContentControl"},
      {ElementKind::ButtonBase, "ButtonBase"},
      {ElementKind::Button, "Button"},
      {ElementKind::ItemsControl, "ItemsControl"},
      {ElementKind::ListView, "ListView"},
      {ElementKind::ScrollViewer, "ScrollViewer"},
      {ElementKind::Slider, "Slider"},
      {ElementKind::AutoSuggestBox, "AutoSuggestBox"},
      {ElementKind::NavigationView, "NavigationView"},
      {ElementKind::TreeView, "TreeView"},
      {ElementKind::MenuBar, "MenuBar"},
    });

    for (auto const& [kind, name] : kExpected)
    {
      CHECK(toString(kind) == name);
      CHECK(elementKindFromString(name) == kind);
    }

    CHECK(toString(ElementKind::NavigationView) == "NavigationView");
    CHECK(elementKindFromString("ListView") == ElementKind::ListView);
    CHECK(elementKindFromString(toString(ElementKind::ScrollViewer)) == ElementKind::ScrollViewer);
    CHECK_FALSE(elementKindFromString("StackPanel").has_value());
    CHECK_FALSE(elementKindFromString("").has_value());
  }

  TEST_CASE("ElementKind - the lattice terminates at FrameworkElement", "[winui][unit][layout]")
  {
    CHECK_FALSE(elementBase(ElementKind::FrameworkElement).has_value());
    CHECK(elementBase(ElementKind::Grid) == ElementKind::Panel);
  }
} // namespace ao::winui::test
