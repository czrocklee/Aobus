// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  enum class PropertyKind : std::uint8_t
  {
    Bool,
    Int,
    Double,
    String,
    Enum,
    StringList,
    Size
  };

  enum class ComponentCategory : std::uint8_t
  {
    Container,
    Decorator,
    Track,
    Playback,
    Status,
    Generic,
    Application,
    Library,
    Layout,
  };

  constexpr std::string_view toString(ComponentCategory category) noexcept
  {
    switch (category)
    {
      case ComponentCategory::Container: return "Containers";
      case ComponentCategory::Decorator: return "Decorators";
      case ComponentCategory::Track: return "Tracks";
      case ComponentCategory::Playback: return "Playback";
      case ComponentCategory::Status: return "Status";
      case ComponentCategory::Generic: return "Generic";
      case ComponentCategory::Application: return "Application";
      case ComponentCategory::Library: return "Library";
      case ComponentCategory::Layout: return "Layout";
    }

    return "Unknown";
  }

  enum class ActionSlot : std::uint8_t
  {
    PrimaryClick,
    PrimaryLongPress,
    SecondaryClick,
    SecondaryLongPress
  };

  using ActionSlotMask = std::uint32_t;

  constexpr ActionSlotMask actionSlotBit(ActionSlot const slot) noexcept
  {
    return 1U << static_cast<std::uint8_t>(slot);
  }

  inline constexpr std::string_view kPrimaryActionProp = "primaryAction";
  inline constexpr std::string_view kSecondaryActionProp = "secondaryAction";
  inline constexpr std::string_view kPrimaryLongPressActionProp = "primaryLongPressAction";
  inline constexpr std::string_view kSecondaryLongPressActionProp = "secondaryLongPressAction";

  inline constexpr std::string_view kOrientationProp = "orientation";
  inline constexpr std::string_view kSpacingProp = "spacing";
  inline constexpr std::string_view kTextProp = "text";
  inline constexpr std::string_view kVariantProp = "variant";
  inline constexpr std::string_view kPlaceholderStyleProp = "placeholderStyle";
  inline constexpr std::string_view kStrokeWidthProp = "strokeWidth";
  inline constexpr std::string_view kGlyphScaleProp = "glyphScale";
  inline constexpr std::string_view kModeProp = "mode";
  inline constexpr std::string_view kCommandProp = "command";

  enum class ActionCapability : std::uint8_t
  {
    RequiresAnchor = 1U << 0U,
    PresentsMenu = 1U << 1U
  };

  using ActionCapabilityMask = std::uint8_t;

  constexpr ActionCapabilityMask actionCapabilityBit(ActionCapability const capability) noexcept
  {
    return static_cast<ActionCapabilityMask>(capability);
  }

  constexpr ActionCapabilityMask operator|(ActionCapability const lhs, ActionCapability const rhs) noexcept
  {
    return actionCapabilityBit(lhs) | actionCapabilityBit(rhs);
  }

  struct DefaultActionBinding final
  {
    ActionSlot slot = ActionSlot::PrimaryClick;
    std::string actionId;
  };

  struct PropertySchema final
  {
    std::string name;
    PropertyKind kind = PropertyKind::String;
    std::string label;
    LayoutValue defaultValue = {};
    std::vector<std::string> enumValues = {};
    std::optional<ActionSlot> optActionSlot = {};
  };

  struct ComponentSchema final
  {
    std::string id;
    std::string displayName;
    ComponentCategory category = ComponentCategory::Generic;
    std::vector<PropertySchema> properties = {};
    std::vector<PropertySchema> layoutProperties = {};
    std::size_t minChildren = 0;
    std::optional<std::size_t> optMaxChildren = {};
    LayoutSurfaceCapabilityMask surfaces = static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Main);
    ActionSlotMask actionSlots = 0;
    std::vector<DefaultActionBinding> defaultActions = {};
    bool persistentState = false;

    constexpr bool allows(ActionSlot const slot) const noexcept { return (actionSlots & actionSlotBit(slot)) != 0; }

    std::string_view defaultAction(ActionSlot slot) const noexcept;
    std::optional<std::string_view> actionId(LayoutNode const& node, ActionSlot slot) const;
    ActionSlotMask boundActionSlots(LayoutNode const& node) const;
    bool hasBoundAction(LayoutNode const& node) const;
  };

  constexpr bool isContainer(ComponentSchema const& schema) noexcept
  {
    return schema.minChildren > 0 || !schema.optMaxChildren || *schema.optMaxChildren > 0;
  }

  struct ComponentSchemaExtension final
  {
    std::vector<PropertySchema> properties = {};
    std::vector<PropertySchema> layoutProperties = {};
    ActionSlotMask actionSlots = 0;
    std::vector<DefaultActionBinding> defaultActions = {};
  };

  struct ActionSchema final
  {
    std::string id;
    std::string label;
    std::string category;
    ActionCapabilityMask capabilities = 0;

    constexpr bool supports(ActionCapability const capability) const noexcept
    {
      auto const bit = actionCapabilityBit(capability);
      return (capabilities & bit) == bit;
    }
  };

  /**
   * @brief The component and action vocabulary an authored layout may use.
   *
   * Component and action entries are inert schema values. Frontend action
   * registries separately own executable callbacks, so validation never gains
   * authority to run an action.
   */
  class LayoutSchema final
  {
  public:
    bool addComponent(ComponentSchema schema);
    bool addSharedComponent(std::string_view id, ComponentSchemaExtension extension = {});
    bool addAction(ActionSchema schema);

    std::span<ComponentSchema const> components() const noexcept { return _components; }
    std::span<ActionSchema const> actions() const noexcept { return _actions; }

    std::optional<ComponentSchema> component(std::string_view id) const;
    std::optional<ActionSchema> action(std::string_view id) const;

    static constexpr std::string_view actionProperty(ActionSlot slot) noexcept
    {
      switch (slot)
      {
        case ActionSlot::PrimaryClick: return kPrimaryActionProp;
        case ActionSlot::PrimaryLongPress: return kPrimaryLongPressActionProp;
        case ActionSlot::SecondaryClick: return kSecondaryActionProp;
        case ActionSlot::SecondaryLongPress: return kSecondaryLongPressActionProp;
      }

      return {};
    }

  private:
    std::vector<ComponentSchema> _components;
    std::vector<ActionSchema> _actions;
    boost::unordered_flat_map<std::string, std::size_t, utility::TransparentStringHash, utility::TransparentStringEqual>
      _componentIndexById;
    boost::unordered_flat_map<std::string, std::size_t, utility::TransparentStringHash, utility::TransparentStringEqual>
      _actionIndexById;
  };

  /// The canonical component entries whose authored meaning is shared by desktop shells.
  std::span<ComponentSchema const> sharedComponentSchemas();
} // namespace ao::uimodel
