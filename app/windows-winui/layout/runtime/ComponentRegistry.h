// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  class LayoutSchema;
  struct LayoutNode;
}

namespace ao::winui::layout
{
  struct LayoutBuildContext;
  class ActionRegistry;

  using ComponentFactory =
    std::function<Result<std::unique_ptr<LayoutComponent>>(LayoutBuildContext&, uimodel::LayoutNode const&)>;

  /**
   * @brief Maps a validated Windows component type to its native construction.
   *
   * The registry only constructs; the schema decides which types exist and the
   * validator has already rejected anything the registry cannot build. A
   * construction failure fails the whole candidate rather than substituting a
   * diagnostic placeholder.
   */
  class ComponentRegistry final
  {
  public:
    ComponentRegistry(uimodel::LayoutSchema const& schema, ActionRegistry const& actions);

    void registerComponent(std::string_view type, ComponentFactory factory);

    uimodel::LayoutSchema const& schema() const noexcept { return _schema; }

    /// Build @p node and its subtree.
    Result<PlacedChild> build(LayoutBuildContext& ctx, uimodel::LayoutNode const& node) const;

  private:
    uimodel::LayoutSchema const& _schema;
    ActionRegistry const& _actions;
    boost::
      unordered_flat_map<std::string, ComponentFactory, utility::TransparentStringHash, utility::TransparentStringEqual>
        _factories;
  };
} // namespace ao::winui::layout
