// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentTooltipController.h"

#include "layout/runtime/ILayoutComponent.h"

#include <gtkmm/widget.h>
#include <sigc++/functors/mem_fun.h>

#include <cstdint>

namespace ao::gtk::layout
{
  ComponentTooltipController::ComponentTooltipController() = default;


  void ComponentTooltipController::attach(Gtk::Widget& target, ILayoutComponent& tooltipComponent)
  {
    _target = &target;
    _tooltipComponent = &tooltipComponent;

    _target->set_has_tooltip(true);
    _target->signal_query_tooltip().connect(sigc::mem_fun(*this, &ComponentTooltipController::onQueryTooltip), false);
  }

  bool ComponentTooltipController::onQueryTooltip(std::int32_t /*xCoord*/,
                                                  std::int32_t /*yCoord*/,
                                                  bool /*keyboardTooltip*/,
                                                  Glib::RefPtr<Gtk::Tooltip> const& tooltip)
  {
    if (_tooltipComponent == nullptr || !_tooltipComponent->widget().get_visible())
    {
      return false;
    }

    // The component's widget must remain persistent and stable across repeated queries.
    tooltip->set_custom(_tooltipComponent->widget());
    return true;
  }
} // namespace ao::gtk::layout
