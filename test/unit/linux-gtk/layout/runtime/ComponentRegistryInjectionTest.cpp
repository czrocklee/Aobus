// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("ComponentRegistry registers descriptors after uimodel action property injection",
            "[gtk][unit][layout][runtime]")
  {
    auto registry = ComponentRegistry{};

    registry.registerComponent({.type = "test.secondary",
                                .displayName = "Secondary",
                                .category = LayoutComponentCategory::Generic,
                                .actionPolicy = uimodel::kExternalSecondaryActions},
                               nullptr);

    auto const optDesc = registry.descriptor("test.secondary");
    REQUIRE(optDesc.has_value());

    auto const it = std::find_if(optDesc->props.begin(),
                                 optDesc->props.end(),
                                 [](auto const& p) { return p.name == uimodel::kSecondaryActionProp; });
    REQUIRE(it != optDesc->props.end());
    REQUIRE(it->optActionBinding.has_value());
    CHECK(it->optActionBinding->slot == uimodel::LayoutActionSlot::SecondaryClick);
  }
} // namespace ao::gtk::layout::test
