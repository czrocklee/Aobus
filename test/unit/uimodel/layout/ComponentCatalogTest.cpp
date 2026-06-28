// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/ComponentCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

namespace ao::uimodel::layout::test
{
  namespace
  {
    PropertyDescriptor const* propertyByName(ComponentDescriptor const& descriptor, std::string_view name)
    {
      auto const it =
        std::ranges::find_if(descriptor.props, [name](PropertyDescriptor const& prop) { return prop.name == name; });

      if (it == descriptor.props.end())
      {
        return nullptr;
      }

      return &*it;
    }
  } // namespace

  TEST_CASE("ComponentCatalog duplicate registration preserves the original descriptor",
            "[uimodel][unit][layout][catalog]")
  {
    auto catalog = ComponentCatalog{};

    SECTION("register and retrieve descriptor")
    {
      auto const registered = catalog.registerComponentDescriptor(
        ComponentDescriptor{.type = "playback.playPauseButton",
                            .displayName = "Play/Pause Button",
                            .category = ComponentCategory::Playback,
                            .props = {{.name = "showLabel", .kind = PropertyKind::Bool, .label = "Show Label"}}});
      CHECK(registered == true);

      auto const optDesc = catalog.descriptor("playback.playPauseButton");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->type == "playback.playPauseButton");
      CHECK(optDesc->displayName == "Play/Pause Button");
      CHECK(optDesc->category == ComponentCategory::Playback);
      CHECK(optDesc->props.size() == 1);
      CHECK(optDesc->props[0].name == "showLabel");
    }

    SECTION("rejects duplicate type")
    {
      CHECK(catalog.registerComponentDescriptor(ComponentDescriptor{
        .type = "box",
        .displayName = "Box",
        .category = ComponentCategory::Container,
        .props = {{.name = "spacing", .kind = PropertyKind::Int, .label = "Spacing"}},
        .layoutProps = {{.name = "grow", .kind = PropertyKind::Bool, .label = "Grow"}},
        .minChildren = 1,
        .optMaxChildren = 2,
        .surfaces = static_cast<SurfaceCapabilityMask>(SurfaceCapability::Main) |
                    static_cast<SurfaceCapabilityMask>(SurfaceCapability::Tooltip),
        .actionPolicy = ComponentActionPolicy{.slotMask = slotBit(ActionSlot::PrimaryClick),
                                              .defaultActionIds = {{ActionSlot::PrimaryClick, "valid.action"}}}}));
      CHECK(catalog.registerComponentDescriptor(ComponentDescriptor{
              .type = "box", .displayName = "Box Duplicate", .category = ComponentCategory::Track, .minChildren = 0}) ==
            false);

      auto const optDesc = catalog.descriptor("box");
      REQUIRE(optDesc.has_value());
      CHECK(optDesc->type == "box");
      CHECK(optDesc->displayName == "Box");
      CHECK(optDesc->category == ComponentCategory::Container);
      REQUIRE(optDesc->props.size() == 1);
      CHECK(optDesc->props.front().name == "spacing");
      CHECK(optDesc->props.front().label == "Spacing");
      REQUIRE(optDesc->layoutProps.size() == 1);
      CHECK(optDesc->layoutProps.front().name == "grow");
      CHECK(optDesc->layoutProps.front().kind == PropertyKind::Bool);
      CHECK(optDesc->minChildren == 1);
      REQUIRE(optDesc->optMaxChildren.has_value());
      CHECK(*optDesc->optMaxChildren == 2);
      CHECK((optDesc->surfaces & static_cast<SurfaceCapabilityMask>(SurfaceCapability::Main)) != 0);
      CHECK((optDesc->surfaces & static_cast<SurfaceCapabilityMask>(SurfaceCapability::Tooltip)) != 0);
      CHECK(optDesc->actionPolicy.allows(ActionSlot::PrimaryClick));
      CHECK(optDesc->actionPolicy.getDefault(ActionSlot::PrimaryClick) == "valid.action");

      auto const& all = catalog.descriptors();
      REQUIRE(all.size() == 1);
      CHECK(all.front().displayName == "Box");
    }

    SECTION("returns nullopt for unknown type")
    {
      auto const optDesc = catalog.descriptor("nonexistent");
      CHECK(optDesc.has_value() == false);
    }

    SECTION("returns descriptors in registration order")
    {
      catalog.registerComponentDescriptor(
        ComponentDescriptor{.type = "a", .displayName = "A", .category = ComponentCategory::Container});
      catalog.registerComponentDescriptor(
        ComponentDescriptor{.type = "b", .displayName = "B", .category = ComponentCategory::Track});

      auto const& all = catalog.descriptors();
      REQUIRE(all.size() == 2);
      CHECK(all[0].type == "a");
      CHECK(all[1].type == "b");
    }
  }

  TEST_CASE("componentDescriptorWithActionProperties injects action binding properties",
            "[uimodel][unit][layout][catalog]")
  {
    SECTION("injects all external action slots with default ids")
    {
      auto descriptor = componentDescriptorWithActionProperties(ComponentDescriptor{
        .type = "test.all",
        .displayName = "All",
        .category = ComponentCategory::Generic,
        .actionPolicy =
          ComponentActionPolicy{.slotMask = kAllExternalActions.slotMask,
                                .defaultActionIds = {{ActionSlot::PrimaryClick, "default.primary"},
                                                     {ActionSlot::SecondaryLongPress, "default.secondaryLong"}}}});

      auto const* primary = propertyByName(descriptor, kPrimaryActionProp);
      REQUIRE(primary != nullptr);
      CHECK(primary->kind == PropertyKind::Enum);
      CHECK(primary->label == "Primary Action");
      REQUIRE(primary->optActionBinding.has_value());
      CHECK(primary->optActionBinding->slot == ActionSlot::PrimaryClick);
      REQUIRE(primary->optDefaultActionId.has_value());
      CHECK(*primary->optDefaultActionId == "default.primary");

      auto const* primaryLong = propertyByName(descriptor, kPrimaryLongPressActionProp);
      REQUIRE(primaryLong != nullptr);
      CHECK(primaryLong->label == "Primary Long Press");
      REQUIRE(primaryLong->optActionBinding.has_value());
      CHECK(primaryLong->optActionBinding->slot == ActionSlot::PrimaryLongPress);
      CHECK_FALSE(primaryLong->optDefaultActionId.has_value());

      auto const* secondary = propertyByName(descriptor, kSecondaryActionProp);
      REQUIRE(secondary != nullptr);
      CHECK(secondary->label == "Secondary Action");
      REQUIRE(secondary->optActionBinding.has_value());
      CHECK(secondary->optActionBinding->slot == ActionSlot::SecondaryClick);

      auto const* secondaryLong = propertyByName(descriptor, kSecondaryLongPressActionProp);
      REQUIRE(secondaryLong != nullptr);
      CHECK(secondaryLong->label == "Secondary Long Press");
      REQUIRE(secondaryLong->optActionBinding.has_value());
      CHECK(secondaryLong->optActionBinding->slot == ActionSlot::SecondaryLongPress);
      REQUIRE(secondaryLong->optDefaultActionId.has_value());
      CHECK(*secondaryLong->optDefaultActionId == "default.secondaryLong");
    }

    SECTION("injects only secondary external actions")
    {
      auto descriptor =
        componentDescriptorWithActionProperties(ComponentDescriptor{.type = "test.secondary",
                                                                    .displayName = "Secondary",
                                                                    .category = ComponentCategory::Generic,
                                                                    .actionPolicy = kExternalSecondaryActions});

      CHECK(propertyByName(descriptor, kPrimaryActionProp) == nullptr);
      CHECK(propertyByName(descriptor, kPrimaryLongPressActionProp) == nullptr);
      CHECK(propertyByName(descriptor, kSecondaryActionProp) != nullptr);
      CHECK(propertyByName(descriptor, kSecondaryLongPressActionProp) != nullptr);
    }

    SECTION("preserves existing properties")
    {
      auto descriptor =
        componentDescriptorWithActionProperties(ComponentDescriptor{.type = "test.overwrite",
                                                                    .displayName = "Overwrite",
                                                                    .category = ComponentCategory::Generic,
                                                                    .props = {{.name = "primaryAction",
                                                                               .kind = PropertyKind::String,
                                                                               .label = "Custom Label",
                                                                               .defaultValue = LayoutValue{"custom"}}},
                                                                    .actionPolicy = kAllExternalActions});

      auto const* primary = propertyByName(descriptor, kPrimaryActionProp);
      REQUIRE(primary != nullptr);
      CHECK(primary->kind == PropertyKind::String);
      CHECK(primary->label == "Custom Label");
      CHECK(primary->defaultValue.asString() == "custom");
    }
  }
} // namespace ao::uimodel::layout::test
