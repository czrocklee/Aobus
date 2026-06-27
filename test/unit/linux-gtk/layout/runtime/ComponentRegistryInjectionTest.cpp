// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/ComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;

  TEST_CASE("ComponentRegistry injects declared action descriptors", "[gtk][unit][layout][runtime]")
  {
    auto registry = ComponentRegistry{};

    SECTION("injects all actions when policy is kAllExternalActions")
    {
      registry.registerComponent({.type = "test.all",
                                  .displayName = "All",
                                  .category = ComponentCategory::Generic,
                                  .actionPolicy = uimodel::layout::kAllExternalActions},
                                 nullptr);

      auto const optDesc = registry.descriptor("test.all");
      REQUIRE(optDesc.has_value());

      auto const hasProp = [&](std::string_view name)
      {
        return std::any_of(
          optDesc->props.begin(), optDesc->props.end(), [name](auto const& p) { return p.name == name; });
      };

      CHECK(hasProp(uimodel::layout::kPrimaryActionProp));
      CHECK(hasProp(uimodel::layout::kPrimaryLongPressActionProp));
      CHECK(hasProp(uimodel::layout::kSecondaryActionProp));
      CHECK(hasProp(uimodel::layout::kSecondaryLongPressActionProp));
    }

    SECTION("injects only secondary actions when policy is kExternalSecondaryActions")
    {
      registry.registerComponent({.type = "test.secondary",
                                  .displayName = "Secondary",
                                  .category = ComponentCategory::Generic,
                                  .actionPolicy = uimodel::layout::kExternalSecondaryActions},
                                 nullptr);

      auto const optDesc = registry.descriptor("test.secondary");
      REQUIRE(optDesc.has_value());

      auto const hasProp = [&](std::string_view name)
      {
        return std::any_of(
          optDesc->props.begin(), optDesc->props.end(), [name](auto const& p) { return p.name == name; });
      };

      CHECK_FALSE(hasProp(uimodel::layout::kPrimaryActionProp));
      CHECK_FALSE(hasProp(uimodel::layout::kPrimaryLongPressActionProp));
      CHECK(hasProp(uimodel::layout::kSecondaryActionProp));
      CHECK(hasProp(uimodel::layout::kSecondaryLongPressActionProp));
    }

    SECTION("injects nothing when policy is kNoExternalActions (default)")
    {
      registry.registerComponent(
        {.type = "test.none", .displayName = "None", .category = ComponentCategory::Generic}, nullptr);

      auto const optDesc = registry.descriptor("test.none");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->props.empty());
    }

    SECTION("does not overwrite existing properties")
    {
      registry.registerComponent(
        {.type = "test.overwrite",
         .displayName = "Overwrite",
         .category = ComponentCategory::Generic,
         .props = {{.name = "primaryAction", .kind = PropertyKind::String, .label = "Custom Label"}},
         .actionPolicy = uimodel::layout::kAllExternalActions},
        nullptr);

      auto const optDesc = registry.descriptor("test.overwrite");
      REQUIRE(optDesc.has_value());

      auto it = std::find_if(
        optDesc->props.begin(), optDesc->props.end(), [](auto const& p) { return p.name == "primaryAction"; });
      REQUIRE(it != optDesc->props.end());
      CHECK(it->label == "Custom Label");
      CHECK(it->kind == PropertyKind::String);
    }
  }
} // namespace ao::gtk::layout::test
