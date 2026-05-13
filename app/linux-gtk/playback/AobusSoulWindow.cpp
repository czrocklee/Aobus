// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulWindow.h"
#include <gdkmm/display.h>
#include <gdkmm/monitor.h>
#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/shortcut.h>
#include <gtkmm/shortcutaction.h>
#include <gtkmm/shortcutcontroller.h>
#include <gtkmm/shortcuttrigger.h>
#include <gtkmm/stylecontext.h>
#include <runtime/AppSession.h>

namespace ao::gtk
{
  namespace
  {
    constexpr int kDefaultLogoHeight = 400;
  } // namespace

  AobusSoulWindow::AobusSoulWindow()
  {
    set_title("Aobus Soul");
    set_name("AobusSoul");
    set_decorated(false);
    set_modal(true);
    set_hide_on_close(true);

    ensureCss();
    add_css_class("soul-window");

    auto* const centerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    centerBox->set_valign(Gtk::Align::CENTER);
    centerBox->set_halign(Gtk::Align::CENTER);

    int logoHeight = kDefaultLogoHeight;
    if (auto const display = Gdk::Display::get_default(); display != nullptr)
    {
      auto const monitors = display->get_monitors();
      if (monitors->get_n_items() > 0)
      {
        if (auto const monitor = std::dynamic_pointer_cast<Gdk::Monitor>(monitors->get_object(0)))
        {
          auto geometry = Gdk::Rectangle{};
          monitor->get_geometry(geometry);
          if (geometry.get_height() > 0)
          {
            logoHeight =
              static_cast<int>(std::round(static_cast<double>(geometry.get_height()) / AobusSoul::kGoldenRatio));
          }
        }
      }
    }

    auto const logoWidth = static_cast<int>(std::round(static_cast<double>(logoHeight) * (147.0 / 65.0)));
    _bigSoul.setShowFullLogo(true);
    _bigSoul.set_size_request(logoWidth, logoHeight);
    _bigSoul.set_halign(Gtk::Align::CENTER);
    _bigSoul.set_valign(Gtk::Align::CENTER);
    centerBox->append(_bigSoul);
    set_child(*centerBox);

    auto const gesture = Gtk::GestureClick::create();
    gesture->signal_released().connect(
      [this](int, double, double)
      {
        Glib::signal_idle().connect(
          [this]
          {
            hide();
            return false;
          });
      });

    add_controller(gesture);

    auto const shortcutController = Gtk::ShortcutController::create();
    auto const trigger = Gtk::ShortcutTrigger::parse_string("Escape");
    auto const action = Gtk::CallbackAction::create(
      [this](Gtk::Widget&, Glib::VariantBase const&) -> bool
      {
        Glib::signal_idle().connect(
          [this]
          {
            hide();
            return false;
          });
        return true;
      });

    shortcutController->add_shortcut(Gtk::Shortcut::create(trigger, action));
    add_controller(shortcutController);

    fullscreen();
  }

  AobusSoulWindow::~AobusSoulWindow()
  {
    _soulBinding.reset();
  }

  void AobusSoulWindow::bind(ao::rt::PlaybackService& playback)
  {
    _playback = &playback;
    if (get_visible())
    {
      _soulBinding = std::make_unique<AobusSoulBinding>(_bigSoul, *_playback);
    }
  }

  void AobusSoulWindow::on_show()
  {
    Gtk::Window::on_show();
    if (_playback != nullptr)
    {
      _soulBinding = std::make_unique<AobusSoulBinding>(_bigSoul, *_playback);
    }
  }

  void AobusSoulWindow::on_hide()
  {
    _soulBinding.reset();
    Gtk::Window::on_hide();
  }

  void AobusSoulWindow::ensureCss()
  {
    [[maybe_unused]] static auto const cssInitialized = []
    {
      auto const provider = Gtk::CssProvider::create();
      provider->load_from_data(".soul-window { background-color: rgba(0, 0, 0, 0.85); }");
      if (auto const display = Gdk::Display::get_default(); display != nullptr)
      {
        Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      }

      return true;
    }();
  }
} // namespace ao::gtk
