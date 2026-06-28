// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>

#include <gtkmm/enums.h>

namespace ao::gtk::layout
{
  LayoutHost::LayoutHost(ComponentRegistry const& registry)
    : _runtime{registry}
  {
    set_orientation(Gtk::Orientation::VERTICAL);
  }

  void LayoutHost::setLayout(LayoutContext& ctx, uimodel::LayoutDocument const& doc)
  {
    // Invalidate any pending runtime-state writes from components that are about to be
    // destroyed. New components built below will capture the updated generation.
    ++ctx.componentStateGeneration;

    if (_activeComponentPtr)
    {
      remove(_activeComponentPtr->widget());
      _activeComponentPtr.reset();
    }

    _activeComponentPtr = _runtime.build(ctx, doc);

    if (_activeComponentPtr)
    {
      auto& activeWidget = _activeComponentPtr->widget();
      activeWidget.set_hexpand(true);
      activeWidget.set_vexpand(true);
      append(activeWidget);
    }
  }
} // namespace ao::gtk::layout
