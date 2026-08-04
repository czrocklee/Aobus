// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"

#include "layout/runtime/ActionBinder.h"
#include "layout/runtime/CommonLayoutProps.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/PlacementPlan.h>

#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui::layout
{
  ComponentRegistry::ComponentRegistry()
  {
    registerContainerComponents(*this);
    registerGenericComponents(*this);
    registerPlaybackComponents(*this);
    registerShellComponents(*this);
    registerStatusComponents(*this);
    registerTrackComponents(*this);
  }

  void ComponentRegistry::registerComponent(std::string_view const type, ComponentFactory factory)
  {
    _factories.insert_or_assign(std::string{type}, std::move(factory));
  }

  Result<PlacedChild> ComponentRegistry::build(LayoutBuildContext& ctx, uimodel::LayoutNode const& node) const
  {
    auto const optKind = winui::componentElementKind(node);
    auto const optDescriptor = ctx.catalog.descriptor(node.type);
    auto const it = _factories.find(node.type);

    if (!optKind || !optDescriptor || it == _factories.end())
    {
      return makeError(
        Error::Code::NotSupported, std::format("No Windows component construction is registered for '{}'", node.type));
    }

    auto componentPtr = it->second(ctx, node);

    if (!componentPtr)
    {
      return std::unexpected{componentPtr.error()};
    }

    if (!*componentPtr)
    {
      return makeError(Error::Code::InitFailed, std::format("Windows component '{}' produced no element", node.type));
    }

    if (!node.children.empty())
    {
      auto* const container = dynamic_cast<LayoutContainer*>(componentPtr->get());

      if (container == nullptr)
      {
        return makeError(
          Error::Code::InvalidState, std::format("Windows component '{}' cannot host children", node.type));
      }

      auto children = std::vector<PlacedChild>{};
      children.reserve(node.children.size());

      for (auto const& child : node.children)
      {
        auto built = build(ctx, child);

        if (!built)
        {
          return std::unexpected{built.error()};
        }

        children.push_back(std::move(*built));
      }

      container->adopt(std::move(children));
    }

    auto const placement = winui::planPlacement(node);
    auto const element = (*componentPtr)->element();
    auto applied = applyCommonProps(element, node, placement, *optKind, ctx.resources, ctx.surfaceBrush);

    if (!applied)
    {
      return std::unexpected{applied.error()};
    }

    // Interaction is bound centrally because the slot policy is a catalog fact:
    // a component decides what it presents, never which gestures it accepts.
    auto bound = bindActions(ctx, node, optDescriptor->actionPolicy, element);

    if (!bound)
    {
      return std::unexpected{bound.error()};
    }

    return PlacedChild{.componentPtr = std::move(*componentPtr), .placement = placement};
  }
} // namespace ao::winui::layout
