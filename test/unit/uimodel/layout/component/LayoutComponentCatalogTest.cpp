// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace ao::uimodel::test
{
  namespace
  {
    LayoutPropertyDescriptor const* propertyByName(LayoutComponentDescriptor const& descriptor, std::string_view name)
    {
      auto const it = std::ranges::find_if(
        descriptor.props, [name](LayoutPropertyDescriptor const& prop) { return prop.name == name; });

      if (it == descriptor.props.end())
      {
        return nullptr;
      }

      return &*it;
    }
  } // namespace

  TEST_CASE("LayoutComponentCatalog - duplicate registration preserves the original descriptor",
            "[uimodel][unit][layout][component]")
  {
    auto catalog = LayoutComponentCatalog{};

    SECTION("register and retrieve descriptor")
    {
      auto const registered = catalog.registerComponentDescriptor(LayoutComponentDescriptor{
        .type = "playback.playPauseButton",
        .displayName = "Play/Pause Button",
        .category = LayoutComponentCategory::Playback,
        .props = {{.name = "showLabel", .kind = LayoutPropertyKind::Bool, .label = "Show Label"}}});
      CHECK(registered == true);

      auto const optDesc = catalog.descriptor("playback.playPauseButton");
      REQUIRE(optDesc);
      CHECK(optDesc->type == "playback.playPauseButton");
      CHECK(optDesc->displayName == "Play/Pause Button");
      CHECK(optDesc->category == LayoutComponentCategory::Playback);
      CHECK(optDesc->props.size() == 1);
      CHECK(optDesc->props[0].name == "showLabel");
    }

    SECTION("rejects duplicate type")
    {
      CHECK(catalog.registerComponentDescriptor(LayoutComponentDescriptor{
        .type = "box",
        .displayName = "Box",
        .category = LayoutComponentCategory::Container,
        .props = {{.name = "spacing", .kind = LayoutPropertyKind::Int, .label = "Spacing"}},
        .layoutProps = {{.name = "grow", .kind = LayoutPropertyKind::Bool, .label = "Grow"}},
        .minChildren = 1,
        .optMaxChildren = 2,
        .surfaces = static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Main) |
                    static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Tooltip),
        .actionPolicy =
          LayoutComponentActionPolicy{.slotMask = slotBit(LayoutActionSlot::PrimaryClick),
                                      .defaultActionIds = {{LayoutActionSlot::PrimaryClick, "valid.action"}}}}));
      CHECK(catalog.registerComponentDescriptor(LayoutComponentDescriptor{.type = "box",
                                                                          .displayName = "Box Duplicate",
                                                                          .category = LayoutComponentCategory::Track,
                                                                          .minChildren = 0}) == false);

      auto const optDesc = catalog.descriptor("box");
      REQUIRE(optDesc);
      CHECK(optDesc->type == "box");
      CHECK(optDesc->displayName == "Box");
      CHECK(optDesc->category == LayoutComponentCategory::Container);
      REQUIRE(optDesc->props.size() == 1);
      CHECK(optDesc->props.front().name == "spacing");
      CHECK(optDesc->props.front().label == "Spacing");
      REQUIRE(optDesc->layoutProps.size() == 1);
      CHECK(optDesc->layoutProps.front().name == "grow");
      CHECK(optDesc->layoutProps.front().kind == LayoutPropertyKind::Bool);
      CHECK(optDesc->minChildren == 1);
      REQUIRE(optDesc->optMaxChildren);
      CHECK(*optDesc->optMaxChildren == 2);
      CHECK((optDesc->surfaces & static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Main)) != 0);
      CHECK((optDesc->surfaces & static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Tooltip)) != 0);
      CHECK(optDesc->actionPolicy.isSlotAllowed(LayoutActionSlot::PrimaryClick));
      CHECK(optDesc->actionPolicy.defaultAction(LayoutActionSlot::PrimaryClick) == "valid.action");

      auto const& all = catalog.descriptors();
      REQUIRE(all.size() == 1);
      CHECK(all.front().displayName == "Box");
    }

    SECTION("returns nullopt for unknown type")
    {
      auto const optDesc = catalog.descriptor("nonexistent");
      CHECK_FALSE(optDesc);
    }

    SECTION("returns descriptors in registration order")
    {
      catalog.registerComponentDescriptor(
        LayoutComponentDescriptor{.type = "a", .displayName = "A", .category = LayoutComponentCategory::Container});
      catalog.registerComponentDescriptor(
        LayoutComponentDescriptor{.type = "b", .displayName = "B", .category = LayoutComponentCategory::Track});

      auto const& all = catalog.descriptors();
      REQUIRE(all.size() == 2);
      CHECK(all[0].type == "a");
      CHECK(all[1].type == "b");
    }
  }

  TEST_CASE("componentDescriptorWithActionProperties injects action binding properties",
            "[uimodel][unit][layout][component]")
  {
    SECTION("injects all external action slots with default ids")
    {
      auto descriptor = componentDescriptorWithActionProperties(LayoutComponentDescriptor{
        .type = "test.all",
        .displayName = "All",
        .category = LayoutComponentCategory::Generic,
        .actionPolicy = LayoutComponentActionPolicy{
          .slotMask = kAllExternalActions.slotMask,
          .defaultActionIds = {{LayoutActionSlot::PrimaryClick, "default.primary"},
                               {LayoutActionSlot::SecondaryLongPress, "default.secondaryLong"}}}});

      auto const* primary = propertyByName(descriptor, kPrimaryActionProp);
      REQUIRE(primary != nullptr);
      CHECK(primary->kind == LayoutPropertyKind::Enum);
      CHECK(primary->label == "Primary Action");
      REQUIRE(primary->optActionBinding);
      CHECK(primary->optActionBinding->slot == LayoutActionSlot::PrimaryClick);
      REQUIRE(primary->optDefaultActionId);
      CHECK(*primary->optDefaultActionId == "default.primary");

      auto const* primaryLong = propertyByName(descriptor, kPrimaryLongPressActionProp);
      REQUIRE(primaryLong != nullptr);
      CHECK(primaryLong->label == "Primary Long Press");
      REQUIRE(primaryLong->optActionBinding);
      CHECK(primaryLong->optActionBinding->slot == LayoutActionSlot::PrimaryLongPress);
      CHECK_FALSE(primaryLong->optDefaultActionId);

      auto const* secondary = propertyByName(descriptor, kSecondaryActionProp);
      REQUIRE(secondary != nullptr);
      CHECK(secondary->label == "Secondary Action");
      REQUIRE(secondary->optActionBinding);
      CHECK(secondary->optActionBinding->slot == LayoutActionSlot::SecondaryClick);

      auto const* secondaryLong = propertyByName(descriptor, kSecondaryLongPressActionProp);
      REQUIRE(secondaryLong != nullptr);
      CHECK(secondaryLong->label == "Secondary Long Press");
      REQUIRE(secondaryLong->optActionBinding);
      CHECK(secondaryLong->optActionBinding->slot == LayoutActionSlot::SecondaryLongPress);
      REQUIRE(secondaryLong->optDefaultActionId);
      CHECK(*secondaryLong->optDefaultActionId == "default.secondaryLong");
    }

    SECTION("injects only secondary external actions")
    {
      auto descriptor =
        componentDescriptorWithActionProperties(LayoutComponentDescriptor{.type = "test.secondary",
                                                                          .displayName = "Secondary",
                                                                          .category = LayoutComponentCategory::Generic,
                                                                          .actionPolicy = kExternalSecondaryActions});

      CHECK(propertyByName(descriptor, kPrimaryActionProp) == nullptr);
      CHECK(propertyByName(descriptor, kPrimaryLongPressActionProp) == nullptr);
      CHECK(propertyByName(descriptor, kSecondaryActionProp) != nullptr);
      CHECK(propertyByName(descriptor, kSecondaryLongPressActionProp) != nullptr);
    }

    SECTION("preserves existing properties")
    {
      auto descriptor = componentDescriptorWithActionProperties(
        LayoutComponentDescriptor{.type = "test.overwrite",
                                  .displayName = "Overwrite",
                                  .category = LayoutComponentCategory::Generic,
                                  .props = {{.name = "primaryAction",
                                             .kind = LayoutPropertyKind::String,
                                             .label = "Custom Label",
                                             .defaultValue = LayoutValue{"custom"}}},
                                  .actionPolicy = kAllExternalActions});

      auto const* primary = propertyByName(descriptor, kPrimaryActionProp);
      REQUIRE(primary != nullptr);
      CHECK(primary->kind == LayoutPropertyKind::String);
      CHECK(primary->label == "Custom Label");
      CHECK(primary->defaultValue.asString() == "custom");
    }
  }
} // namespace ao::uimodel::test
