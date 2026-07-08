// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::layout
{
  using ComponentFactory = std::unique_ptr<LayoutComponent> (*)(LayoutContext&, uimodel::LayoutNode const&);

  class ComponentRegistry final
  {
  public:
    ComponentRegistry();

    void registerComponent(uimodel::LayoutComponentDescriptor descriptor, ComponentFactory factory);

    std::unique_ptr<LayoutComponent> create(LayoutContext& ctx, uimodel::LayoutNode const& node) const;

    std::vector<uimodel::LayoutComponentDescriptor> const& descriptors() const;

    std::optional<uimodel::LayoutComponentDescriptor> descriptor(std::string_view type) const;

    uimodel::LayoutComponentCatalog const& catalog() const noexcept;

  private:
    uimodel::LayoutComponentCatalog _catalog;
    boost::
      unordered_flat_map<std::string, ComponentFactory, utility::TransparentStringHash, utility::TransparentStringEqual>
        _factories;
  };
} // namespace ao::gtk::layout
