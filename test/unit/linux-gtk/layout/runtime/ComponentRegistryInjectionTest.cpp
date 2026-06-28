// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/ComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ao::gtk::layout::test
{
  using namespace uimodel::layout;

  TEST_CASE("ComponentRegistry registers descriptors after uimodel action property injection",
            "[gtk][unit][layout][runtime]")
  {
    auto registry = ComponentRegistry{};

    registry.registerComponent({.type = "test.secondary",
                                .displayName = "Secondary",
                                .category = ComponentCategory::Generic,
                                .actionPolicy = uimodel::layout::kExternalSecondaryActions},
                               nullptr);

    auto const optDesc = registry.descriptor("test.secondary");
    REQUIRE(optDesc.has_value());

    auto const it = std::find_if(optDesc->props.begin(),
                                 optDesc->props.end(),
                                 [](auto const& p) { return p.name == uimodel::layout::kSecondaryActionProp; });
    REQUIRE(it != optDesc->props.end());
    REQUIRE(it->optActionBinding.has_value());
    CHECK(it->optActionBinding->slot == uimodel::layout::ActionSlot::SecondaryClick);
  }
} // namespace ao::gtk::layout::test
