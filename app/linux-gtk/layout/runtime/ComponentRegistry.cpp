// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistry.h"

#include "layout/component/common/CommonLayoutProps.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentInteractionController.h"
#include "layout/runtime/DecoratedLayoutComponent.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/Contract.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  namespace
  {
    class ErrorComponent final : public LayoutComponent
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

  void ComponentRegistry::registerComponent(uimodel::ComponentSchema schema, ComponentFactory factory)
  {
    auto const type = schema.id;

    // Preserve the established test/composition seam: once a schema is
    // authoritative, a later registration for that type replaces only the
    // native factory. The schema cannot drift through this path.
    if (_schema.component(type))
    {
      _factories[type] = std::move(factory);
      return;
    }

    auto const added = _schema.addComponent(std::move(schema));
    AO_EXPECTS(added, "A component registration must provide one unique valid schema entry");

    if (!added)
    {
      return;
    }

    _factories[type] = std::move(factory);
  }

  void ComponentRegistry::registerSharedComponent(std::string_view const id,
                                                  uimodel::ComponentSchemaExtension extension,
                                                  ComponentFactory factory)
  {
    // Preserve the registry's established replacement seam: tests and narrow
    // composition roots may replace a factory while the first schema entry
    // remains authoritative.
    if (!_schema.component(id))
    {
      auto const added = _schema.addSharedComponent(id, std::move(extension));
      AO_EXPECTS(added, "A shared component registration must import a valid canonical schema entry");

      if (!added)
      {
        return;
      }
    }

    _factories[std::string{id}] = std::move(factory);
  }

  void ComponentRegistry::registerSharedComponent(std::string_view const id, ComponentFactory factory)
  {
    registerSharedComponent(id, {}, std::move(factory));
  }

  std::unique_ptr<LayoutComponent> ComponentRegistry::create(LayoutBuildContext& ctx,
                                                             uimodel::LayoutNode const& node) const
  {
    auto componentPtr = std::unique_ptr<LayoutComponent>{};
    auto const optComponentSchema = _schema.component(node.type);

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
    componentPtr->onAuthoredPropsApplied();

    // Phase 2: Automatic interaction controller attachment
    auto interactionControllerPtr = std::unique_ptr<ComponentInteractionController>{};

    if (optComponentSchema && ctx.surface != uimodel::LayoutSurface::Tooltip)
    {
      if (optComponentSchema->hasBoundAction(node))
      {
        interactionControllerPtr = std::make_unique<ComponentInteractionController>();
        interactionControllerPtr->attach(ctx, node, componentPtr->widget(), *optComponentSchema);
      }
    }

    auto tooltipComponentPtr = std::unique_ptr<LayoutComponent>{};

    if (node.optTooltip && node.optTooltip->nodePtr)
    {
      struct [[nodiscard]] SurfaceGuard
      {
        LayoutBuildContext& ctx;
        uimodel::LayoutSurface saved;

        SurfaceGuard(LayoutBuildContext& ctxRef, uimodel::LayoutSurface surface)
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

      ctx.surface = uimodel::LayoutSurface::Tooltip;

      // Ignore nested tooltips when already building a tooltip surface.
      if (guard.saved != uimodel::LayoutSurface::Tooltip)
      {
        tooltipComponentPtr = create(ctx, *node.optTooltip->nodePtr);

        if (tooltipComponentPtr)
        {
          // applyCommonProps is now handled inside create()
        }
      }

      if (tooltipComponentPtr)
      {
        return std::make_unique<DecoratedLayoutComponent>(std::move(componentPtr),
                                                          std::move(tooltipComponentPtr),
                                                          ctx.timeoutScheduler,
                                                          std::move(interactionControllerPtr));
      }
    }

    if (interactionControllerPtr)
    {
      return std::make_unique<DecoratedLayoutComponent>(std::move(componentPtr),
                                                        std::move(tooltipComponentPtr),
                                                        ctx.timeoutScheduler,
                                                        std::move(interactionControllerPtr));
    }

    return componentPtr;
  }
} // namespace ao::gtk::layout
