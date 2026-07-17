// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionBinding.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ao::uimodel
{
  enum class LayoutPropertyKind : std::uint8_t
  {
    Bool,
    Int,
    Double,
    String,
    Enum,
    StringList,
    Size
  };

  inline constexpr std::string_view kPrimaryActionProp = "primaryAction";
  inline constexpr std::string_view kSecondaryActionProp = "secondaryAction";
  inline constexpr std::string_view kPrimaryLongPressActionProp = "primaryLongPressAction";
  inline constexpr std::string_view kSecondaryLongPressActionProp = "secondaryLongPressAction";

  struct LayoutPropertyDescriptor final
  {
    std::string name;
    LayoutPropertyKind kind = LayoutPropertyKind::String;
    std::string label;
    LayoutValue defaultValue = {};
    std::vector<std::string> enumValues = {};
    std::optional<LayoutActionBindingProperty> optActionBinding = {};
    std::optional<std::string> optDefaultActionId = {};
  };

  enum class LayoutComponentCategory : std::uint8_t
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

  constexpr std::string_view toString(LayoutComponentCategory category) noexcept
  {
    switch (category)
    {
      case LayoutComponentCategory::Container: return "Containers";
      case LayoutComponentCategory::Decorator: return "Decorators";
      case LayoutComponentCategory::Track: return "Tracks";
      case LayoutComponentCategory::Playback: return "Playback";
      case LayoutComponentCategory::Status: return "Status";
      case LayoutComponentCategory::Generic: return "Generic";
      case LayoutComponentCategory::Application: return "Application";
      case LayoutComponentCategory::Library: return "Library";
      case LayoutComponentCategory::Layout: return "Layout";
    }

    return "Unknown";
  }

  enum class LayoutSurfaceCapability : std::uint8_t
  {
    Main = 1,
    Tooltip = 2,
  };

  using LayoutSurfaceCapabilityMask = std::underlying_type_t<LayoutSurfaceCapability>;

  struct LayoutComponentDescriptor final
  {
    std::string type;
    std::string displayName;
    LayoutComponentCategory category = LayoutComponentCategory::Generic;
    std::vector<LayoutPropertyDescriptor> props = {};
    std::vector<LayoutPropertyDescriptor> layoutProps = {};
    std::size_t minChildren = 0;
    std::optional<std::size_t> optMaxChildren = {};
    LayoutSurfaceCapabilityMask surfaces = static_cast<LayoutSurfaceCapabilityMask>(LayoutSurfaceCapability::Main);
    LayoutComponentActionPolicy actionPolicy = kNoExternalActions;
  };

  constexpr bool isContainer(LayoutComponentDescriptor const& descriptor) noexcept
  {
    // A component is a container if it can have at least one child.
    return descriptor.minChildren > 0 || !descriptor.optMaxChildren || *descriptor.optMaxChildren > 0;
  }

  LayoutComponentDescriptor componentDescriptorWithActionProperties(LayoutComponentDescriptor descriptor);

  class LayoutComponentCatalog final
  {
  public:
    bool registerComponentDescriptor(LayoutComponentDescriptor descriptor);

    std::vector<LayoutComponentDescriptor> const& descriptors() const noexcept;

    std::optional<LayoutComponentDescriptor> descriptor(std::string_view type) const;

  private:
    std::vector<LayoutComponentDescriptor> _descriptors = {};
    boost::unordered_flat_map<std::string, std::size_t, utility::TransparentStringHash, utility::TransparentStringEqual>
      _descriptorIndexByType = {};
  };
} // namespace ao::uimodel
