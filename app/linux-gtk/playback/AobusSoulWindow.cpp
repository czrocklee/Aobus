// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulWindow.h"

#include "app/AobusSoul.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/soul/AobusSoulViewModel.h>

#include <gdkmm/monitor.h>
#include <gdkmm/rectangle.h>
#include <glibmm/main.h>
#include <glibmm/refptr.h>
#include <glibmm/variant.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/object.h>
#include <gtkmm/shortcut.h>
#include <gtkmm/shortcutaction.h>
#include <gtkmm/shortcutcontroller.h>
#include <gtkmm/shortcuttrigger.h>
#include <gtkmm/window.h>
#include <sigc++/adaptors/track_obj.h>

#include <cmath>
#include <cstdint>
#include <memory>

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

    add_css_class("ao-soul-window");

    auto* const centerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    centerBox->set_valign(Gtk::Align::CENTER);
    centerBox->set_halign(Gtk::Align::CENTER);

    std::int32_t logoHeight = kDefaultLogoHeight;

    if (auto const displayPtr = Gdk::Display::get_default(); displayPtr != nullptr)
    {
      if (auto const monitorsPtr = displayPtr->get_monitors(); monitorsPtr->get_n_items() > 0)
      {
        if (auto const monitorPtr = std::dynamic_pointer_cast<Gdk::Monitor>(monitorsPtr->get_object(0));
            monitorPtr != nullptr)
        {
          auto geometry = Gdk::Rectangle{};
          monitorPtr->get_geometry(geometry);

          if (geometry.get_height() > 0)
          {
            logoHeight = static_cast<std::int32_t>(
              std::round(static_cast<double>(geometry.get_height()) / AobusSoul::kGoldenRatio));
          }
        }
      }
    }

    auto const logoWidth = static_cast<std::int32_t>(std::round(static_cast<double>(logoHeight) * (147.0 / 65.0)));
    _bigSoul.setShowFullLogo(true);
    _bigSoul.set_size_request(logoWidth, logoHeight);
    _bigSoul.set_halign(Gtk::Align::CENTER);
    _bigSoul.set_valign(Gtk::Align::CENTER);
    centerBox->append(_bigSoul);
    set_child(*centerBox);

    auto const gesturePtr = Gtk::GestureClick::create();
    gesturePtr->signal_released().connect(
      [this](std::int32_t, double, double)
      { Glib::signal_idle().connect_once(sigc::track_object([this] { hide(); }, *this)); });

    add_controller(gesturePtr);

    auto const shortcutControllerPtr = Gtk::ShortcutController::create();
    auto const triggerPtr = Gtk::ShortcutTrigger::parse_string("Escape");
    auto const actionPtr = Gtk::CallbackAction::create(
      [this](Gtk::Widget&, Glib::VariantBase const&) -> bool
      {
        Glib::signal_idle().connect_once(sigc::track_object([this] { hide(); }, *this));
        return true;
      });

    shortcutControllerPtr->add_shortcut(Gtk::Shortcut::create(triggerPtr, actionPtr));
    add_controller(shortcutControllerPtr);

    fullscreen();
  }

  AobusSoulWindow::~AobusSoulWindow() = default;

  void AobusSoulWindow::bind(rt::PlaybackService& playback)
  {
    _playback = &playback;

    if (get_visible())
    {
      _soulViewModelPtr =
        std::make_unique<ao::uimodel::AobusSoulViewModel>(*_playback,
                                                          [this](ao::uimodel::AobusSoulViewState const& view)
                                                          {
                                                            _bigSoul.breathe(view.isBreathing);
                                                            _bigSoul.setAura(AobusSoul::mapSoulAura(view.aura));
                                                          });
    }
  }

  void AobusSoulWindow::on_show()
  {
    Gtk::Window::on_show();

    if (_playback != nullptr)
    {
      _soulViewModelPtr =
        std::make_unique<ao::uimodel::AobusSoulViewModel>(*_playback,
                                                          [this](ao::uimodel::AobusSoulViewState const& view)
                                                          {
                                                            _bigSoul.breathe(view.isBreathing);
                                                            _bigSoul.setAura(AobusSoul::mapSoulAura(view.aura));
                                                          });
    }
  }

  void AobusSoulWindow::on_hide()
  {
    _soulViewModelPtr.reset();
    Gtk::Window::on_hide();
  }
} // namespace ao::gtk
