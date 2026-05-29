// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::layout
{
  enum class PropertyKind : std::uint8_t
  {
    Bool,
    Int,
    Double,
    String,
    Enum,
    StringList,
    CssClassList,
    Size
  };

  struct PropertyDescriptor final
  {
    std::string name;
    PropertyKind kind = PropertyKind::String;
    std::string label;
    LayoutValue defaultValue = {};
    std::vector<std::string> enumValues = {};
    std::optional<ActionBindingProperty> optActionBinding = {};
  };

  struct ComponentDescriptor final
  {
    std::string type;
    std::string displayName;
    std::string category;
    bool container = false;
    std::vector<PropertyDescriptor> props = {};
    std::vector<PropertyDescriptor> layoutProps = {};
    std::size_t minChildren = 0;
    std::optional<std::size_t> optMaxChildren = {};
  };

  class ComponentCatalog final
  {
  public:
    bool registerComponentDescriptor(ComponentDescriptor descriptor);

    std::vector<ComponentDescriptor> const& descriptors() const noexcept;

    std::optional<ComponentDescriptor> descriptor(std::string_view type) const;

  private:
    std::vector<ComponentDescriptor> _descriptors;
    std::map<std::string, std::size_t, std::less<>> _descriptorIndexMap;
  };
}
