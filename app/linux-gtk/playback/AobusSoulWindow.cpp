// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulWindow.h"
#include <gdkmm/display.h>
#include <gdkmm/frameclock.h>
#include <gdkmm/monitor.h>
#include <gtkmm/box.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/shortcut.h>
#include <gtkmm/shortcutaction.h>
#include <gtkmm/shortcutcontroller.h>
#include <gtkmm/shortcuttrigger.h>
#include <gtkmm/stylecontext.h>
#include <mutex>

namespace ao::gtk
{
  AobusSoulWindow::AobusSoulWindow()
  {
    set_title("Aobus Soul");
    set_name("AobusSoul"); // Help WM identify the window
    set_decorated(false);  // No title bar
    set_modal(true);

    ensureCss();
    add_css_class("soul-window");

    // Center the giant soul
    auto* const centerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    centerBox->set_valign(Gtk::Align::CENTER);
    centerBox->set_halign(Gtk::Align::CENTER);

    // Calculate Golden Size based on monitor height
    int logoHeight = kDefaultLogoHeight; // Fallback
    if (auto const display = Gdk::Display::get_default(); display != nullptr)
    {
      auto const monitors = display->get_monitors();
      if (monitors->get_n_items() > 0)
      {
        if (auto const monitor = std::dynamic_pointer_cast<Gdk::Monitor>(monitors->get_object(0)))
        {
          auto geometry = Gdk::Rectangle{};
          monitor->get_geometry(geometry);
          // Golden Ratio: Height / kGoldenRatio
          if (geometry.get_height() > 0)
          {
            logoHeight =
              static_cast<int>(std::round(static_cast<double>(geometry.get_height()) / AobusSoul::kGoldenRatio));
          }
        }
      }
    }

    int const logoWidth = static_cast<int>(std::round(static_cast<double>(logoHeight) * (147.0 / 65.0)));
    _bigSoul.setShowFullLogo(true);
    _bigSoul.set_size_request(logoWidth, logoHeight);
    _bigSoul.set_halign(Gtk::Align::CENTER);
    _bigSoul.set_valign(Gtk::Align::CENTER);
    centerBox->append(_bigSoul);
    set_child(*centerBox);

    // Close on click
    auto const gesture = Gtk::GestureClick::create();
    gesture->signal_pressed().connect([this](int, double, double) { hide(); });
    add_controller(gesture);

    // Close on Escape or any key
    auto const shortcutController = Gtk::ShortcutController::create();
    auto const trigger = Gtk::ShortcutTrigger::parse_string("Escape");
    auto const action = Gtk::CallbackAction::create(
      [this](Gtk::Widget&, Glib::VariantBase const&) -> bool
      {
        hide();
        return true;
      });
    shortcutController->add_shortcut(Gtk::Shortcut::create(trigger, action));
    add_controller(shortcutController);

    fullscreen();

    // Tick for animation
    _tickId = add_tick_callback(
      [this](Glib::RefPtr<Gdk::FrameClock> const& clock) -> bool
      {
        static constexpr double kMicrosecondsPerSecond = 1'000'000.0;
        auto const frameTime = clock->get_frame_time();

        if (_firstFrameTime == 0)
        {
          _firstFrameTime = frameTime;
        }

        _animationTime = static_cast<double>(frameTime - _firstFrameTime) / kMicrosecondsPerSecond;
        _bigSoul.update(_animationTime, _currentQuality, !_isPlaying, true);
        return true;
      });
  }

  AobusSoulWindow::~AobusSoulWindow()
  {
    if (_tickId != 0)
    {
      remove_tick_callback(_tickId);
    }
  }

  void AobusSoulWindow::updateState(ao::audio::Quality quality, bool isPlaying)
  {
    _currentQuality = quality;
    _isPlaying = isPlaying;
  }

  void AobusSoulWindow::ensureCss()
  {
    [[maybe_unused]] static auto const cssInitialized = []
    {
      auto const provider = Gtk::CssProvider::create();
      provider->load_from_data(".soul-window {"
                               "  background-color: rgba(0, 0, 0, 0.85);"
                               "}");
      if (auto const display = Gdk::Display::get_default(); display != nullptr)
      {
        Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      }

      return true;
    }();
  }
} // namespace ao::gtk
