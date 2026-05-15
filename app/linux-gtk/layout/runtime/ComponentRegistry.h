// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "layout/document/LayoutNode.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutDependencies.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::layout
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

  using ComponentFactory = std::unique_ptr<ILayoutComponent> (*)(LayoutDependencies&, LayoutNode const&);

  /**
   * @brief Registry for layout component types and their metadata.
   */
  class ComponentRegistry final
  {
  public:
    ComponentRegistry();

    /**
     * @brief Register a factory and metadata for a component type.
     */
    void registerComponent(ComponentDescriptor descriptor, ComponentFactory factory);

    /**
     * @brief Create a component instance for a given node.
     *
     * Returns an error placeholder component if the type is unknown.
     */
    std::unique_ptr<ILayoutComponent> create(LayoutDependencies& ctx, LayoutNode const& node) const;

    /**
     * @brief Get all registered component descriptors.
     */
    std::vector<ComponentDescriptor> const& getDescriptors() const;

    /**
     * @brief Get a descriptor by component type.
     */
    std::optional<ComponentDescriptor> getDescriptor(std::string const& type) const;

  private:
    std::map<std::string, ComponentFactory, std::less<>> _factories;
    std::vector<ComponentDescriptor> _descriptors;
    std::map<std::string, std::size_t, std::less<>> _descriptorIndexMap;
  };
}
