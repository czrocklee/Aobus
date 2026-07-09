// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/main.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

namespace ao::gtk::layout
{
  class LayoutComponent;

  class ComponentTooltipController final
  {
  public:
    ComponentTooltipController();
    ~ComponentTooltipController();

    ComponentTooltipController(ComponentTooltipController const&) = delete;
    ComponentTooltipController& operator=(ComponentTooltipController const&) = delete;
    ComponentTooltipController(ComponentTooltipController&&) = delete;
    ComponentTooltipController& operator=(ComponentTooltipController&&) = delete;

    void attach(Gtk::Widget& target, LayoutComponent& tooltipComponent);

  private:
    void detach();
    void handlePointerEntered();
    void handlePointerLeft();

    Gtk::Widget* _target = nullptr;
    LayoutComponent* _tooltipComponent = nullptr;

    Gtk::Popover _popover;
    Glib::RefPtr<Gtk::EventControllerMotion> _motionControllerPtr;
    sigc::connection _motionPointerEnteredConn;
    sigc::connection _motionPointerLeftConn;
    sigc::connection _hoverTimeout;
  };
} // namespace ao::gtk::layout
