// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControlWidget.h"

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/output/VolumeViewModel.h>

#include <gdk/gdk.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/object.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace ao::gtk
{
  VolumeControlWidget::VolumeControlWidget(rt::PlaybackService& playbackService)
    : _volumeViewModel{playbackService, [this](ao::uimodel::VolumeViewState const& state) { applyState(state); }}
  {
    _button.set_child(_icon);
    _button.set_valign(Gtk::Align::CENTER);
    _button.set_has_frame(false);
    _button.add_css_class("ao-volume-button-modern");

    // Gesture for clicks
    auto const clickGesturePtr = Gtk::GestureClick::create();
    clickGesturePtr->set_button(0); // Listen to all buttons
    clickGesturePtr->signal_pressed().connect(
      [this, clickGesturePtr](std::int32_t /*n_press*/, double /*x*/, double /*y*/)
      {
        if (auto const button = clickGesturePtr->get_current_button(); button == GDK_BUTTON_PRIMARY)
        {
          _popover.popup();
        }
        else if (button == GDK_BUTTON_MIDDLE)
        {
          _volumeViewModel.toggleMuted();
        }
      },
      false);
    _button.add_controller(clickGesturePtr);

    // Scroll controller
    auto const scrollControllerPtr = Gtk::EventControllerScroll::create();
    scrollControllerPtr->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    scrollControllerPtr->signal_scroll().connect(
      [this](double /*dx*/, double dy)
      {
        _volumeViewModel.handleScroll(dy);

        // Show bubble
        if (!_popover.get_visible())
        {
          _scrollBubble.popup();
        }

        if (_scrollBubbleTimeout)
        {
          _scrollBubbleTimeout.disconnect();
        }

        constexpr auto kScrollBubbleTimeout = std::chrono::milliseconds{500};
        _scrollBubbleTimeout = Glib::signal_timeout().connect(
          [this]
          {
            _scrollBubble.popdown();
            return false;
          },
          kScrollBubbleTimeout.count());

        return true;
      },
      false);
    _button.add_controller(scrollControllerPtr);

    // Setup Precision Popover
    _popover.set_parent(_button);
    _popover.set_position(Gtk::PositionType::TOP);
    _popover.set_autohide(true);
    _popover.set_has_arrow(true);
    _popover.add_css_class("ao-volume-popover");

    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    vbox->set_margin(4);

    _valueLabel.set_halign(Gtk::Align::CENTER);
    vbox->append(_valueLabel);

    _scale.set_range(0.0, 1.0);
    constexpr double kScaleStepIncrement = 0.02;
    constexpr double kScalePageIncrement = 0.1;
    constexpr int kScaleHeight = 160;
    _scale.set_increments(kScaleStepIncrement, kScalePageIncrement);
    _scale.set_inverted(true); // Top is louder
    _scale.set_draw_value(false);
    _scale.set_vexpand(true);
    _scale.set_size_request(32, kScaleHeight);
    _scale.add_css_class("ao-volume-scale");

    _scale.signal_value_changed().connect(
      [this]
      {
        if (_updating)
        {
          return;
        }

        _volumeViewModel.handleVolumeChanged(static_cast<float>(_scale.get_value()));
      });
    vbox->append(_scale);

    _muteButton.set_icon_name("audio-volume-muted-symbolic");
    _muteButton.set_tooltip_text("Toggle Mute");
    _muteButton.set_halign(Gtk::Align::CENTER);
    _muteButton.signal_toggled().connect(
      [this]
      {
        if (_updating)
        {
          return;
        }

        _volumeViewModel.handleMutedChanged(_muteButton.get_active());
      });
    vbox->append(_muteButton);

    _popover.set_child(*vbox);

    // Setup Scroll Bubble
    _scrollBubble.set_parent(_button);
    _scrollBubble.set_position(Gtk::PositionType::TOP);
    _scrollBubble.set_autohide(false);
    _scrollBubble.set_has_arrow(false);
    _scrollBubble.set_can_target(false); // Don't block clicks
    _scrollBubble.add_css_class("ao-volume-scroll-bubble");
    _scrollBubble.set_child(_scrollBubbleLabel);
  }

  VolumeControlWidget::~VolumeControlWidget()
  {
    if (_scrollBubbleTimeout)
    {
      _scrollBubbleTimeout.disconnect();
    }

    _popover.unparent();
    _scrollBubble.unparent();
  }

  void VolumeControlWidget::applyState(uimodel::VolumeViewState const& view)
  {
    _button.set_visible(view.visible);

    if (view.visible)
    {
      _updating = true;
      _button.set_tooltip_text(view.tooltip);
      _icon.set_from_icon_name(view.iconName);

      _scale.set_value(view.volume);
      _muteButton.set_active(view.muted);

      std::int32_t const percent = static_cast<std::int32_t>(std::round(view.volume * 100.0F));
      std::string const percentText = std::to_string(percent) + "%";
      _valueLabel.set_text(percentText);
      _scrollBubbleLabel.set_text(percentText);

      _updating = false;
    }
  }

  void VolumeControlWidget::setOrientation(Gtk::Orientation /*orientation*/)
  {
    // No-op for modern control
  }
} // namespace ao::gtk
