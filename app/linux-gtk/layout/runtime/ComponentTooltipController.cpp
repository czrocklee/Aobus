// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentTooltipController.h"

#include "layout/runtime/LayoutComponent.h"

#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/widget.h>

#include <chrono>
#include <string_view>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr std::string_view kPopoverShellClassPrefix = "ao-popover-";
    constexpr auto kDefaultHoverDelay = std::chrono::milliseconds{500};
  } // namespace

  ComponentTooltipController::ComponentTooltipController() = default;

  ComponentTooltipController::~ComponentTooltipController()
  {
    detach();
  }

  void ComponentTooltipController::attach(Gtk::Widget& target, LayoutComponent& tooltipComponent)
  {
    detach();

    _target = &target;
    _tooltipComponent = &tooltipComponent;

    // Configure the popover
    _popover.set_parent(target);
    _popover.set_child(tooltipComponent.widget());
    _popover.set_has_arrow(false);
    _popover.set_autohide(false);
    _popover.set_position(Gtk::PositionType::BOTTOM);
    _popover.add_css_class("ao-layout-tooltip-popover");

    // Popover shell utilities target the GtkPopover CSS node; generic utility
    // classes must stay on the tooltip content widget.
    for (auto const& cssClass : tooltipComponent.widget().get_css_classes())
    {
      if (auto const& cssClassRaw = cssClass.raw();
          std::string_view{cssClassRaw.data(), cssClassRaw.size()}.starts_with(kPopoverShellClassPrefix))
      {
        _popover.add_css_class(cssClass);
      }
    }

    // Set up hover interaction
    _motionControllerPtr = Gtk::EventControllerMotion::create();
    _motionEnterConn = _motionControllerPtr->signal_enter().connect([this](double, double) { onEnter(); });
    _motionLeaveConn = _motionControllerPtr->signal_leave().connect([this] { onLeave(); });
    target.add_controller(_motionControllerPtr);
  }

  void ComponentTooltipController::detach()
  {
    _hoverTimeout.disconnect();
    _motionEnterConn.disconnect();
    _motionLeaveConn.disconnect();

    if (_target != nullptr && _motionControllerPtr)
    {
      _target->remove_controller(_motionControllerPtr);
    }

    _motionControllerPtr = nullptr;

    if (_popover.get_parent() != nullptr)
    {
      _popover.unparent();
    }

    _target = nullptr;
    _tooltipComponent = nullptr;
  }

  void ComponentTooltipController::onEnter()
  {
    _hoverTimeout.disconnect();

    _hoverTimeout = Glib::signal_timeout().connect(
      [this] -> bool
      {
        if (_tooltipComponent != nullptr && _tooltipComponent->widget().get_visible())
        {
          _popover.popup();
        }

        return false; // run once
      },
      kDefaultHoverDelay.count());
  }

  void ComponentTooltipController::onLeave()
  {
    _hoverTimeout.disconnect();
    _popover.popdown();
  }
} // namespace ao::gtk::layout
