// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ILayoutComponent.h"

#include <glibmm/main.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

namespace ao::gtk::layout
{
  class ComponentTooltipController final
  {
  public:
    ComponentTooltipController();
    ~ComponentTooltipController();

    ComponentTooltipController(ComponentTooltipController const&) = delete;
    ComponentTooltipController& operator=(ComponentTooltipController const&) = delete;
    ComponentTooltipController(ComponentTooltipController&&) = delete;
    ComponentTooltipController& operator=(ComponentTooltipController&&) = delete;

    void attach(Gtk::Widget& target, ILayoutComponent& tooltipComponent);

  private:
    void onEnter();
    void onLeave();

    Gtk::Widget* _target = nullptr;
    ILayoutComponent* _tooltipComponent = nullptr;

    Gtk::Popover _popover;
    Glib::RefPtr<Gtk::EventControllerMotion> _motionControllerPtr;
    sigc::connection _hoverTimeout;
  };
} // namespace ao::gtk::layout
