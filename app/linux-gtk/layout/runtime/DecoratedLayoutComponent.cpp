// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/DecoratedLayoutComponent.h"

#include "layout/runtime/ILayoutComponent.h"

#include <gtkmm/widget.h>

#include <memory>
#include <utility>

namespace ao::gtk::layout
{
  DecoratedLayoutComponent::DecoratedLayoutComponent(std::unique_ptr<ILayoutComponent> contentPtr,
                                                     std::unique_ptr<ILayoutComponent> tooltipPtr)
    : _contentPtr{std::move(contentPtr)}, _tooltipPtr{std::move(tooltipPtr)}
  {
    if (_contentPtr && _tooltipPtr)
    {
      _tooltipController.attach(_contentPtr->widget(), *_tooltipPtr);
    }
  }

  Gtk::Widget& DecoratedLayoutComponent::widget()
  {
    // The tooltip widget is only used by the tooltip controller via query-tooltip.
    // The primary widget of this component is always the content widget.
    return _contentPtr->widget();
  }
} // namespace ao::gtk::layout
