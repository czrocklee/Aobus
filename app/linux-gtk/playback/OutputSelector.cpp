// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputSelector.h"

#include <ao/uimodel/playback/AobusSoulViewModel.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gdk/gdk.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturelongpress.h>

#include <cstdint>

namespace ao::gtk
{
  namespace
  {
    constexpr double kLongPressDelayFactor = 2.0;
  }

  OutputSelector::OutputSelector(rt::PlaybackService& playback)
    : _playback{playback}
    , _soulController{_playback,
                      [this](ao::uimodel::playback::AobusSoulViewState const& view)
                      {
                        _soul.breathe(view.isBreathing);
                        _soul.setAura(AobusSoul::mapAuraColor(view.auraColor));
                      }}
    , _tooltipViewModel{_playback,
                        [this](ao::uimodel::playback::NowPlayingViewState const& view)
                        { _richTooltipController.apply(view.audioQualityTooltip); }}
  {
    _richTooltipController.attach(_button);
    _button.set_has_frame(false);
    _button.add_css_class("ao-output-logo");
    _button.set_halign(Gtk::Align::START);
    _button.set_valign(Gtk::Align::CENTER);
    _button.set_child(_soul);

    _button.signal_clicked().connect(
      [this]
      {
        if (_longPressHandled)
        {
          _longPressHandled = false;
          return;
        }

        if (_actions.onPrimaryClick)
        {
          _actions.onPrimaryClick();
        }
      });

    auto const secondaryClickPtr = Gtk::GestureClick::create();
    secondaryClickPtr->set_button(GDK_BUTTON_SECONDARY);
    secondaryClickPtr->signal_released().connect(
      [this](std::int32_t, double, double)
      {
        if (_longPressHandled)
        {
          _longPressHandled = false;
          return;
        }

        if (_actions.onSecondaryClick)
        {
          _actions.onSecondaryClick();
        }
      });
    _button.add_controller(secondaryClickPtr);

    auto const primaryLongPressPtr = Gtk::GestureLongPress::create();
    primaryLongPressPtr->set_button(GDK_BUTTON_PRIMARY);
    primaryLongPressPtr->set_delay_factor(kLongPressDelayFactor);
    primaryLongPressPtr->signal_pressed().connect(
      [this](double, double)
      {
        _longPressHandled = true;

        if (_actions.onPrimaryLongPress)
        {
          _actions.onPrimaryLongPress();
        }
      });
    _button.add_controller(primaryLongPressPtr);

    auto const secondaryLongPressPtr = Gtk::GestureLongPress::create();
    secondaryLongPressPtr->set_button(GDK_BUTTON_SECONDARY);
    secondaryLongPressPtr->set_delay_factor(kLongPressDelayFactor);
    secondaryLongPressPtr->signal_pressed().connect(
      [this](double, double)
      {
        _longPressHandled = true;

        if (_actions.onSecondaryLongPress)
        {
          _actions.onSecondaryLongPress();
        }
      });
    _button.add_controller(secondaryLongPressPtr);
  }

  OutputSelector::~OutputSelector() = default;
} // namespace ao::gtk
