// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "LayoutHost.h"
#include "components/Containers.h"

namespace ao::gtk::layout
{
  LayoutHost::LayoutHost(ComponentRegistry const& registry)
    : _runtime{registry}
  {
    set_orientation(Gtk::Orientation::VERTICAL);
  }

  void LayoutHost::setLayout(ComponentContext& ctx, LayoutDocument const& doc)
  {
    if (_activeComponent)
    {
      remove(_activeComponent->widget());
      _activeComponent.reset();
    }

    _activeComponent = _runtime.build(ctx, doc);

    if (_activeComponent)
    {
      applyCommonProps(_activeComponent->widget(), doc.root);
      append(_activeComponent->widget());
    }
  }
} // namespace ao::gtk::layout
