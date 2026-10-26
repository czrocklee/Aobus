// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

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

    auto const expectations =
      std::to_array({std::pair{uimodel::kSecondaryActionProp, ActionSlot::SecondaryClick},
                     std::pair{uimodel::kSecondaryLongPressActionProp, ActionSlot::SecondaryLongPress}});

    for (auto const& [propertyName, expectedSlot] : expectations)
    {
      auto const it = std::ranges::find(optComponentSchema->properties, propertyName, &PropertySchema::name);
      REQUIRE(it != optComponentSchema->properties.end());
      CHECK(it->kind == PropertyKind::Enum);
      CHECK(it->defaultValue.asString().empty());
      CHECK(it->enumValues.empty());
      REQUIRE(it->optActionSlot);
      CHECK(*it->optActionSlot == expectedSlot);

      auto node = LayoutNode{.type = "test.secondary"};
      node.props[std::string{propertyName}] = LayoutValue{std::string{"shell.dynamicAction"}};
      auto const optActionId = optComponentSchema->actionId(node, expectedSlot);
      REQUIRE(optActionId);
      CHECK(*optActionId == "shell.dynamicAction");
    }
  }
} // namespace ao::gtk::layout::test
