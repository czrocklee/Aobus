// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  struct LayoutBuildContext;

  using ComponentFactory =
    std::function<std::unique_ptr<LayoutComponent>(LayoutBuildContext&, uimodel::LayoutNode const&)>;

  class ComponentRegistry final
  {
  public:
    void registerComponent(uimodel::ComponentSchema schema, ComponentFactory factory);
    void registerSharedComponent(std::string_view id,
                                 uimodel::ComponentSchemaExtension extension,
                                 ComponentFactory factory);
    void registerSharedComponent(std::string_view id, ComponentFactory factory);

    std::unique_ptr<LayoutComponent> create(LayoutBuildContext& ctx, uimodel::LayoutNode const& node) const;

    uimodel::LayoutSchema& schema() noexcept { return _schema; }
    uimodel::LayoutSchema const& schema() const noexcept { return _schema; }

  private:
    uimodel::LayoutSchema _schema;
    boost::
      unordered_flat_map<std::string, ComponentFactory, utility::TransparentStringHash, utility::TransparentStringEqual>
        _factories;
  };
} // namespace ao::gtk::layout
