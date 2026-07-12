// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/document/LayoutDocument.h>

#include <gtkmm/enums.h>

namespace ao::gtk::layout
{
  LayoutHost::LayoutHost(ComponentRegistry const& registry)
    : _runtime{registry}
  {
    set_orientation(Gtk::Orientation::VERTICAL);
  }

  void LayoutHost::setLayout(LayoutBuildContext& ctx, uimodel::LayoutDocument const& doc)
  {
    // Invalidate any pending runtime-state writes from components that are about to be
    // destroyed. New components built below will capture the updated generation.
    ++ctx.runtimeState.componentStateGeneration;

    clearLayout();

    _activeComponentPtr = _runtime.build(ctx, doc);

    if (_activeComponentPtr)
    {
      auto& activeWidget = _activeComponentPtr->widget();
      activeWidget.set_hexpand(true);
      activeWidget.set_vexpand(true);
      append(activeWidget);
    }
  }

  void LayoutHost::clearLayout()
  {
    if (_activeComponentPtr)
    {
      remove(_activeComponentPtr->widget());
      _activeComponentPtr.reset();
    }
  }
} // namespace ao::gtk::layout
