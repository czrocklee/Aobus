// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/StyleLookup.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/layout/ElementKind.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

namespace ao::winui::test
{
  namespace
  {
    uimodel::LayoutNode styledNode(uimodel::LayoutValue styleKey)
    {
      return uimodel::LayoutNode{.type = "actionButton", .layout = {{"styleKey", std::move(styleKey)}}};
    }
  } // namespace

  TEST_CASE("planStyleLookup - an authored key plans a lookup for the constructed element", "[winui][unit][layout]")
  {
    auto const optPlan =
      planStyleLookup(styledNode(uimodel::LayoutValue{std::string{"ChromeLessButtonStyle"}}), ElementKind::Button);

    REQUIRE(optPlan);
    CHECK(optPlan->key == "ChromeLessButtonStyle");
    CHECK(optPlan->elementKind == ElementKind::Button);
  }

  TEST_CASE("planStyleLookup - a node without a usable key plans nothing", "[winui][unit][layout]")
  {
    CHECK_FALSE(planStyleLookup(uimodel::LayoutNode{.type = "actionButton"}, ElementKind::Button).has_value());
    CHECK_FALSE(planStyleLookup(styledNode(uimodel::LayoutValue{std::string{}}), ElementKind::Button).has_value());
  }

  TEST_CASE("resolveStyle - a key resolves only against the window's own resources", "[winui][unit][layout]")
  {
    auto const optPlan =
      planStyleLookup(styledNode(uimodel::LayoutValue{std::string{"ChromeLessButtonStyle"}}), ElementKind::Button);

    CHECK(resolveStyle(optPlan, StyleScope::RootGridResources, ElementKind::Button) == StyleResolution::Applied);
    // A key that only exists framework-wide is out of scope, not a silent fallback.
    CHECK(resolveStyle(optPlan, StyleScope::ApplicationResources, ElementKind::Button) == StyleResolution::MissingKey);
    CHECK(resolveStyle(optPlan, StyleScope::Unresolved, std::nullopt) == StyleResolution::MissingKey);
  }

  TEST_CASE("resolveStyle - a style applies through its base target types", "[winui][unit][layout]")
  {
    auto const optPlan =
      planStyleLookup(styledNode(uimodel::LayoutValue{std::string{"ChromeLessButtonStyle"}}), ElementKind::Button);

    CHECK(resolveStyle(optPlan, StyleScope::RootGridResources, ElementKind::ContentControl) ==
          StyleResolution::Applied);
    CHECK(resolveStyle(optPlan, StyleScope::RootGridResources, ElementKind::ListView) ==
          StyleResolution::IncompatibleTarget);
    CHECK(resolveStyle(optPlan, StyleScope::RootGridResources, ElementKind::Border) ==
          StyleResolution::IncompatibleTarget);
  }

  TEST_CASE("resolveStyle - an unstyled node reports no style rather than a defect", "[winui][unit][layout]")
  {
    CHECK(resolveStyle(std::nullopt, StyleScope::Unresolved, std::nullopt) == StyleResolution::NoStyleAuthored);
  }
} // namespace ao::winui::test
