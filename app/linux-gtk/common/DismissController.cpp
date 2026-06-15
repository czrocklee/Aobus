// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/DismissController.h"

#include <gtkmm/eventcontroller.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <utility>

namespace ao::gtk
{
  DismissController::~DismissController()
  {
    remove();
  }

  void DismissController::install(Gtk::Widget& rootedWidget,
                                  std::initializer_list<Gtk::Widget*> insideWidgets,
                                  std::function<void()> onDismiss)
  {
    if (_clickPtr)
    {
      return;
    }

    auto* window = dynamic_cast<Gtk::Window*>(rootedWidget.get_root());

    if (window == nullptr)
    {
      window = dynamic_cast<Gtk::Window*>(&rootedWidget);
    }

    if (window == nullptr)
    {
      return;
    }

    _watchedWindow = window;
    _insideWidgets.assign(insideWidgets.begin(), insideWidgets.end());
    _onDismiss = std::move(onDismiss);

    _clickPtr = Gtk::GestureClick::create();
    _clickPtr->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    _clickPtr->signal_pressed().connect(
      [this](std::int32_t, double const xPos, double const yPos)
      {
        if (_watchedWindow == nullptr)
        {
          return;
        }

        if (pressIsInside(_watchedWindow->pick(xPos, yPos)))
        {
          return;
        }

        if (auto callback = _onDismiss; callback)
        {
          callback();
        }
      });
    _watchedWindow->add_controller(_clickPtr);
  }

  void DismissController::remove()
  {
    if (_clickPtr && _watchedWindow != nullptr)
    {
      _watchedWindow->remove_controller(_clickPtr);
    }

    _clickPtr = nullptr;
    _watchedWindow = nullptr;
    _insideWidgets.clear();
    _onDismiss = {};
  }

  bool DismissController::pressIsInside(Gtk::Widget const* target) const
  {
    for (; target != nullptr; target = target->get_parent())
    {
      for (auto const* const insideWidget : _insideWidgets)
      {
        if (target == insideWidget)
        {
          return true;
        }
      }
    }

    return false;
  }
} // namespace ao::gtk
