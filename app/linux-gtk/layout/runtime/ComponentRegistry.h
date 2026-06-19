// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  using ComponentFactory = std::unique_ptr<ILayoutComponent> (*)(LayoutContext&, uimodel::layout::LayoutNode const&);

  class ComponentRegistry final
  {
  public:
    ComponentRegistry();

    void registerComponent(uimodel::layout::ComponentDescriptor descriptor, ComponentFactory factory);

    std::unique_ptr<ILayoutComponent> create(LayoutContext& ctx, uimodel::layout::LayoutNode const& node) const;

    std::vector<uimodel::layout::ComponentDescriptor> const& descriptors() const;

    std::optional<uimodel::layout::ComponentDescriptor> descriptor(std::string_view type) const;

    uimodel::layout::ComponentCatalog const& catalog() const noexcept;

  private:
    uimodel::layout::ComponentCatalog _catalog;
    boost::
      unordered_flat_map<std::string, ComponentFactory, utility::TransparentStringHash, utility::TransparentStringEqual>
        _factories;
  };
}
