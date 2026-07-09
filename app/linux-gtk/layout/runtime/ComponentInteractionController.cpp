// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentInteractionController.h"

#include "layout/runtime/ActionBinder.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gdk/gdk.h>
#include <gtkmm/button.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>

#include <cstdint>

namespace ao::gtk::layout
{
  namespace
  {
    constexpr double kLongPressDelayFactor = 2.0;
  }

  ComponentInteractionController::ComponentInteractionController() = default;

  ComponentInteractionController::~ComponentInteractionController()
  {
    _primaryClickConn.disconnect();
    _secondaryClickConn.disconnect();
    _primaryLongPressConn.disconnect();
    _secondaryLongPressConn.disconnect();
  }

  void ComponentInteractionController::attach(LayoutContext& ctx,
                                              uimodel::LayoutNode const& node,
                                              Gtk::Widget& target,
                                              uimodel::LayoutComponentActionPolicy const policy)
  {
    auto const binder = ActionBinder{ctx.actionRegistry, ctx.runtime, ctx.parentWindow};

    attachPrimaryClick(node, target, policy, binder);
    attachSecondaryClick(node, target, policy, binder);
    attachPrimaryLongPress(node, target, policy, binder);
    attachSecondaryLongPress(node, target, policy, binder);
  }

  void ComponentInteractionController::attachPrimaryClick(uimodel::LayoutNode const& node,
                                                          Gtk::Widget& target,
                                                          uimodel::LayoutComponentActionPolicy const& policy,
                                                          ActionBinder const& binder)
  {
    if (!policy.isSlotAllowed(uimodel::LayoutActionSlot::PrimaryClick))
    {
      return;
    }

    _primaryClick = binder.bind(node,
                                uimodel::kPrimaryActionProp,
                                policy.defaultAction(uimodel::LayoutActionSlot::PrimaryClick),
                                uimodel::LayoutActionSlot::PrimaryClick,
                                target);

    if (!_primaryClick)
    {
      return;
    }

    if (auto* const button = dynamic_cast<Gtk::Button*>(&target); button != nullptr)
    {
      _primaryClickConn = button->signal_clicked().connect(
        [this]
        {
          if (_primaryLongPressHandled)
          {
            _primaryLongPressHandled = false;
            return;
          }

          if (_primaryClick)
          {
            _primaryClick();
          }
        });
    }
  }

  void ComponentInteractionController::attachSecondaryClick(uimodel::LayoutNode const& node,
                                                            Gtk::Widget& target,
                                                            uimodel::LayoutComponentActionPolicy const& policy,
                                                            ActionBinder const& binder)
  {
    if (!policy.isSlotAllowed(uimodel::LayoutActionSlot::SecondaryClick))
    {
      return;
    }

    _secondaryClick = binder.bind(node,
                                  uimodel::kSecondaryActionProp,
                                  policy.defaultAction(uimodel::LayoutActionSlot::SecondaryClick),
                                  uimodel::LayoutActionSlot::SecondaryClick,
                                  target);

    if (!_secondaryClick)
    {
      return;
    }

    _secondaryClickGesturePtr = Gtk::GestureClick::create();
    _secondaryClickGesturePtr->set_button(GDK_BUTTON_SECONDARY);
    _secondaryClickConn = _secondaryClickGesturePtr->signal_released().connect(
      [this](std::int32_t /*n_press*/, double /*x*/, double /*y*/)
      {
        if (_secondaryLongPressHandled)
        {
          _secondaryLongPressHandled = false;
          return;
        }

        if (_secondaryClick)
        {
          _secondaryClick();
        }
      });
    target.add_controller(_secondaryClickGesturePtr);
  }

  void ComponentInteractionController::attachPrimaryLongPress(uimodel::LayoutNode const& node,
                                                              Gtk::Widget& target,
                                                              uimodel::LayoutComponentActionPolicy const& policy,
                                                              ActionBinder const& binder)
  {
    if (!policy.isSlotAllowed(uimodel::LayoutActionSlot::PrimaryLongPress))
    {
      return;
    }

    _primaryLongPress = binder.bind(node,
                                    uimodel::kPrimaryLongPressActionProp,
                                    policy.defaultAction(uimodel::LayoutActionSlot::PrimaryLongPress),
                                    uimodel::LayoutActionSlot::PrimaryLongPress,
                                    target);

    if (!_primaryLongPress)
    {
      return;
    }

    _primaryLongPressGesturePtr = Gtk::GestureLongPress::create();
    _primaryLongPressGesturePtr->set_button(GDK_BUTTON_PRIMARY);
    _primaryLongPressGesturePtr->set_delay_factor(kLongPressDelayFactor);
    _primaryLongPressConn = _primaryLongPressGesturePtr->signal_pressed().connect(
      [this](double /*x*/, double /*y*/)
      {
        _primaryLongPressHandled = true;

        if (_primaryLongPress)
        {
          _primaryLongPress();
        }
      });
    target.add_controller(_primaryLongPressGesturePtr);
  }

  void ComponentInteractionController::attachSecondaryLongPress(uimodel::LayoutNode const& node,
                                                                Gtk::Widget& target,
                                                                uimodel::LayoutComponentActionPolicy const& policy,
                                                                ActionBinder const& binder)
  {
    if (!policy.isSlotAllowed(uimodel::LayoutActionSlot::SecondaryLongPress))
    {
      return;
    }

    _secondaryLongPress = binder.bind(node,
                                      uimodel::kSecondaryLongPressActionProp,
                                      policy.defaultAction(uimodel::LayoutActionSlot::SecondaryLongPress),
                                      uimodel::LayoutActionSlot::SecondaryLongPress,
                                      target);

    if (!_secondaryLongPress)
    {
      return;
    }

    _secondaryLongPressGesturePtr = Gtk::GestureLongPress::create();
    _secondaryLongPressGesturePtr->set_button(GDK_BUTTON_SECONDARY);
    _secondaryLongPressGesturePtr->set_delay_factor(kLongPressDelayFactor);
    _secondaryLongPressConn = _secondaryLongPressGesturePtr->signal_pressed().connect(
      [this](double /*x*/, double /*y*/)
      {
        _secondaryLongPressHandled = true;

        if (_secondaryLongPress)
        {
          _secondaryLongPress();
        }
      });
    target.add_controller(_secondaryLongPressGesturePtr);
  }
} // namespace ao::gtk::layout
