// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("ComponentRegistry - registers schema entries after uimodel action property injection",
            "[gtk][unit][layout][runtime]")
  {
    auto registry = ComponentRegistry{};

    registry.registerComponent(
      {.id = "test.secondary",
       .displayName = "Secondary",
       .category = ComponentCategory::Generic,
       .actionSlots = actionSlotBit(ActionSlot::SecondaryClick) | actionSlotBit(ActionSlot::SecondaryLongPress)},
      nullptr);

    auto const optComponentSchema = registry.schema().component("test.secondary");
    REQUIRE(optComponentSchema);

    auto const it = std::find_if(optComponentSchema->properties.begin(),
                                 optComponentSchema->properties.end(),
                                 [](auto const& p) { return p.name == uimodel::kSecondaryActionProp; });
    REQUIRE(it != optComponentSchema->properties.end());
    REQUIRE(it->optActionSlot);
    CHECK(*it->optActionSlot == uimodel::ActionSlot::SecondaryClick);
  }
} // namespace ao::gtk::layout::test
