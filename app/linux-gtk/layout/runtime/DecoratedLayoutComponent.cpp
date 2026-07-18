// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/DecoratedLayoutComponent.h"

#include "layout/runtime/ComponentInteractionController.h"
#include "layout/runtime/LayoutComponent.h"

#include <gtkmm/widget.h>

#include <memory>
#include <utility>

namespace ao::gtk::layout
{
  DecoratedLayoutComponent::DecoratedLayoutComponent(std::unique_ptr<LayoutComponent> contentPtr,
                                                     std::unique_ptr<LayoutComponent> tooltipPtr,
                                                     std::unique_ptr<ComponentInteractionController> interactionPtr)
    : _contentPtr{std::move(contentPtr)}, _tooltipPtr{std::move(tooltipPtr)}, _interactionPtr{std::move(interactionPtr)}
  {
    if (_contentPtr && _tooltipPtr)
    {
      _tooltipController.attach(_contentPtr->widget(), *_tooltipPtr);
    }
  }

  DecoratedLayoutComponent::~DecoratedLayoutComponent()
  {
    // Detach interactions while the target content is explicitly known to be alive.
    if (_interactionPtr)
    {
      _interactionPtr->detach();
    }
  }

  Gtk::Widget& DecoratedLayoutComponent::widget()
  {
    // The tooltip widget is only used by the tooltip controller via query-tooltip.
    // The primary widget of this component is always the content widget.
    return _contentPtr->widget();
  }
} // namespace ao::gtk::layout
