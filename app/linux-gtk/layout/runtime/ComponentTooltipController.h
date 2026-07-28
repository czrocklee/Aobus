// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/main.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <functional>

namespace ao::gtk::layout
{
  class LayoutComponent;

  class ComponentTooltipController final
  {
  public:
    using TimeoutScheduler = std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)>;

    explicit ComponentTooltipController(TimeoutScheduler timeoutScheduler = {});
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
    void handleTooltipVisibilityChanged();

    Gtk::Widget* _target = nullptr;
    LayoutComponent* _tooltipComponent = nullptr;

    Gtk::Popover _popover;
    Glib::RefPtr<Gtk::EventControllerMotion> _motionControllerPtr;
    sigc::connection _motionPointerEnteredConn;
    sigc::connection _motionPointerLeftConn;
    sigc::connection _tooltipVisibilityChangedConn;
    sigc::connection _hoverTimeout;
    TimeoutScheduler _timeoutScheduler;
  };
} // namespace ao::gtk::layout
