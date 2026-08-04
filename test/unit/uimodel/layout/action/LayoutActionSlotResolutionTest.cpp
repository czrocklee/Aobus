// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors
#include <ao/uimodel/layout/action/LayoutActionSlotResolution.h>

#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutNode nodeWith(std::string_view const prop, std::string action = "playback.togglePlayPause")
    {
      auto node = LayoutNode{};
      node.type = "playback.playPauseButton";
      node.props[std::string{prop}] = LayoutValue{std::move(action)};
      return node;
    }

    LayoutComponentActionPolicy policyWithDefault(LayoutActionSlot const slot, std::string action)
    {
      auto policy = kAllExternalActions;
      policy.defaultActionIds = {{slot, std::move(action)}};
      return policy;
    }
  } // namespace

  TEST_CASE("actionPropForSlot - maps every slot to its authored property name", "[uimodel][unit][layout][action]")
  {
    CHECK(actionPropForSlot(LayoutActionSlot::PrimaryClick) == kPrimaryActionProp);
    CHECK(actionPropForSlot(LayoutActionSlot::PrimaryLongPress) == kPrimaryLongPressActionProp);
    CHECK(actionPropForSlot(LayoutActionSlot::SecondaryClick) == kSecondaryActionProp);
    CHECK(actionPropForSlot(LayoutActionSlot::SecondaryLongPress) == kSecondaryLongPressActionProp);
  }

  TEST_CASE("isActionSlotBound - an authored property binds an allowed slot", "[uimodel][unit][layout][action]")
  {
    auto const node = nodeWith(kPrimaryActionProp);

    CHECK(isActionSlotBound(kAllExternalActions, node, LayoutActionSlot::PrimaryClick));
    CHECK_FALSE(isActionSlotBound(kAllExternalActions, node, LayoutActionSlot::SecondaryClick));
  }

  TEST_CASE("isActionSlotBound - a disallowed slot stays unbound however it is authored",
            "[uimodel][unit][layout][action]")
  {
    auto const node = nodeWith(kPrimaryActionProp);

    CHECK_FALSE(isActionSlotBound(kNoExternalActions, node, LayoutActionSlot::PrimaryClick));
    CHECK_FALSE(isActionSlotBound(kExternalSecondaryActions, node, LayoutActionSlot::PrimaryClick));

    // A policy default cannot revive a slot the mask excludes either.
    auto policy = policyWithDefault(LayoutActionSlot::PrimaryClick, "shell.showSoul");
    policy.slotMask = kExternalSecondaryActions.slotMask;
    CHECK_FALSE(isActionSlotBound(policy, LayoutNode{}, LayoutActionSlot::PrimaryClick));
  }

  TEST_CASE("isActionSlotBound - a nonempty policy default binds an unauthored slot", "[uimodel][unit][layout][action]")
  {
    auto const policy = policyWithDefault(LayoutActionSlot::SecondaryClick, "shell.showSystemMenu");

    CHECK(isActionSlotBound(policy, LayoutNode{}, LayoutActionSlot::SecondaryClick));
    CHECK_FALSE(isActionSlotBound(policy, LayoutNode{}, LayoutActionSlot::PrimaryClick));
  }

  TEST_CASE("isActionSlotBound - an empty policy default leaves the slot unbound", "[uimodel][unit][layout][action]")
  {
    auto const policy = policyWithDefault(LayoutActionSlot::PrimaryClick, "");

    CHECK_FALSE(isActionSlotBound(policy, LayoutNode{}, LayoutActionSlot::PrimaryClick));
  }

  TEST_CASE("resolveLayoutActionId - an authored unbind suppresses a policy default",
            "[uimodel][regression][layout][action]")
  {
    auto const policy = policyWithDefault(LayoutActionSlot::PrimaryClick, "playback.togglePlayPause");

    for (auto const& actionId : {std::string{}, std::string{"none"}})
    {
      auto const node = nodeWith(kPrimaryActionProp, actionId);

      CHECK_FALSE(resolveLayoutActionId(policy, node, LayoutActionSlot::PrimaryClick).has_value());
      CHECK_FALSE(isActionSlotBound(policy, node, LayoutActionSlot::PrimaryClick));
    }
  }

  TEST_CASE("resolveLayoutActionId - resolves authored and default action ids", "[uimodel][unit][layout][action]")
  {
    auto const policy = policyWithDefault(LayoutActionSlot::PrimaryClick, "playback.togglePlayPause");
    auto const authored = nodeWith(kPrimaryActionProp, "shell.showSoul");

    CHECK(resolveLayoutActionId(policy, authored, LayoutActionSlot::PrimaryClick) == "shell.showSoul");
    CHECK(resolveLayoutActionId(policy, LayoutNode{}, LayoutActionSlot::PrimaryClick) == "playback.togglePlayPause");
  }

  TEST_CASE("boundActionSlots - reports every resolving slot", "[uimodel][unit][layout][action]")
  {
    auto node = nodeWith(kPrimaryActionProp);
    node.props[std::string{kSecondaryLongPressActionProp}] = LayoutValue{std::string{"shell.showSoul"}};

    auto const policy = policyWithDefault(LayoutActionSlot::SecondaryClick, "shell.showSystemMenu");
    auto const mask = boundActionSlots(policy, node);

    CHECK((mask & slotBit(LayoutActionSlot::PrimaryClick)) != 0);
    CHECK((mask & slotBit(LayoutActionSlot::SecondaryClick)) != 0);
    CHECK((mask & slotBit(LayoutActionSlot::SecondaryLongPress)) != 0);
    CHECK((mask & slotBit(LayoutActionSlot::PrimaryLongPress)) == 0);
  }

  TEST_CASE("hasBoundActionSlot - decides whether a node needs interaction wiring", "[uimodel][unit][layout][action]")
  {
    CHECK_FALSE(hasBoundActionSlot(kNoExternalActions, nodeWith(kPrimaryActionProp)));
    CHECK_FALSE(hasBoundActionSlot(kAllExternalActions, LayoutNode{}));
    CHECK(hasBoundActionSlot(kAllExternalActions, nodeWith(kPrimaryActionProp)));
    CHECK(hasBoundActionSlot(policyWithDefault(LayoutActionSlot::PrimaryLongPress, "shell.showSoul"), LayoutNode{}));
  }
} // namespace ao::uimodel::test
