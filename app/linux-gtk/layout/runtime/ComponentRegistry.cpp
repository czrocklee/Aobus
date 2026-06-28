// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"

#include "layout/component/common/CommonLayoutProps.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentInteractionController.h"
#include "layout/runtime/DecoratedLayoutComponent.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ActionTypes.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    class ErrorComponent final : public ILayoutComponent
    {
    public:
      explicit ErrorComponent(std::string const& message)
      {
        _label.set_markup("<span foreground='red'><b>[Layout Error]</b></span> " + message);
        _label.add_css_class("ao-layout-error");
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };
  } // namespace

  ComponentRegistry::ComponentRegistry() = default;

  void ComponentRegistry::registerComponent(uimodel::layout::ComponentDescriptor descriptor, ComponentFactory factory)
  {
    descriptor = uimodel::layout::componentDescriptorWithActionProperties(std::move(descriptor));
    auto const type = std::string{descriptor.type};
    _factories[type] = factory;
    _catalog.registerComponentDescriptor(std::move(descriptor));
  }

  std::unique_ptr<ILayoutComponent> ComponentRegistry::create(LayoutContext& ctx,
                                                              uimodel::layout::LayoutNode const& node) const
  {
    auto componentPtr = std::unique_ptr<ILayoutComponent>{};
    auto const optCompDesc = descriptor(node.type);

    if (auto const it = _factories.find(node.type); it != _factories.end())
    {
      componentPtr = it->second(ctx, node);
    }
    else
    {
      componentPtr = std::make_unique<ErrorComponent>("Unknown component type: " + node.type);
    }

    if (!componentPtr)
    {
      return nullptr;
    }

    applyCommonProps(componentPtr->widget(), node);

    // Phase 2: Automatic interaction controller attachment
    auto interactionControllerPtr = std::unique_ptr<ComponentInteractionController>{};

    if (optCompDesc && ctx.surface != LayoutSurface::Tooltip)
    {
      auto const& policy = optCompDesc->actionPolicy;

      bool hasActions = false;

      auto const check = [&](std::string_view propName, uimodel::layout::ActionSlot slot)
      {
        if (!policy.allows(slot))
        {
          return false;
        }

        if (node.props.contains(std::string{propName}))
        {
          return true;
        }

        return !policy.getDefault(slot).empty();
      };

      hasActions =
        check(uimodel::layout::kPrimaryActionProp, uimodel::layout::ActionSlot::PrimaryClick) ||
        check(uimodel::layout::kPrimaryLongPressActionProp, uimodel::layout::ActionSlot::PrimaryLongPress) ||
        check(uimodel::layout::kSecondaryActionProp, uimodel::layout::ActionSlot::SecondaryClick) ||
        check(uimodel::layout::kSecondaryLongPressActionProp, uimodel::layout::ActionSlot::SecondaryLongPress);

      if (hasActions)
      {
        interactionControllerPtr = std::make_unique<ComponentInteractionController>();
        interactionControllerPtr->attach(ctx, node, componentPtr->widget(), policy);
      }
    }

    auto tooltipComponentPtr = std::unique_ptr<ILayoutComponent>{};

    if (node.optTooltip && node.optTooltip->nodePtr)
    {
      struct [[nodiscard]] SurfaceGuard
      {
        LayoutContext& ctx;
        LayoutSurface saved;

        SurfaceGuard(LayoutContext& ctxRef, LayoutSurface surface)
          : ctx{ctxRef}, saved{surface}
        {
        }
        SurfaceGuard(SurfaceGuard const&) = delete;
        SurfaceGuard& operator=(SurfaceGuard const&) = delete;
        SurfaceGuard(SurfaceGuard&&) = delete;
        SurfaceGuard& operator=(SurfaceGuard&&) = delete;
        ~SurfaceGuard() { ctx.surface = saved; }
      };

      auto const guard = SurfaceGuard{ctx, ctx.surface};

      ctx.surface = LayoutSurface::Tooltip;

      auto tooltipComponentPtr = std::unique_ptr<ILayoutComponent>{};

      // Ignore nested tooltips when already building a tooltip surface.
      if (guard.saved != LayoutSurface::Tooltip)
      {
        tooltipComponentPtr = create(ctx, *node.optTooltip->nodePtr);

        if (tooltipComponentPtr)
        {
          // applyCommonProps is now handled inside create()
        }
      }

      if (tooltipComponentPtr)
      {
        return std::make_unique<DecoratedLayoutComponent>(
          std::move(componentPtr), std::move(tooltipComponentPtr), std::move(interactionControllerPtr));
      }
    }

    if (interactionControllerPtr)
    {
      return std::make_unique<DecoratedLayoutComponent>(
        std::move(componentPtr), std::move(tooltipComponentPtr), std::move(interactionControllerPtr));
    }

    return componentPtr;
  }

  std::vector<uimodel::layout::ComponentDescriptor> const& ComponentRegistry::descriptors() const
  {
    return _catalog.descriptors();
  }

  std::optional<uimodel::layout::ComponentDescriptor> ComponentRegistry::descriptor(std::string_view type) const
  {
    return _catalog.descriptor(type);
  }

  uimodel::layout::ComponentCatalog const& ComponentRegistry::catalog() const noexcept
  {
    return _catalog;
  }
} // namespace ao::gtk::layout
