// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "shell/StatusNotificationLabel.h"
#include <gdkmm/display.h>
#include <glibmm/main.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>
#include <runtime/AppSession.h>
#include <runtime/NotificationService.h>

namespace ao::gtk
{
  namespace
  {
    void ensureStatusNotificationCss()
    {
      static auto const provider = Gtk::CssProvider::create();
      static bool initialized = false;

      if (!initialized)
      {
        provider->load_from_data(R"(
          .status-message {
            /* Styles for status messages in the stack */
          }
        )");

        if (auto display = Gdk::Display::get_default(); display)
        {
          Gtk::StyleContext::add_provider_for_display(display, provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
        }
        initialized = true;
      }
    }
  }

  StatusNotificationLabel::StatusNotificationLabel(ao::rt::NotificationService& notificationService,
                                                   ao::rt::ViewService& viewService)
    : _notificationService{notificationService}, _selectionInfo{viewService}
  {
    ensureStatusNotificationCss();
    _stack.set_transition_type(Gtk::StackTransitionType::SLIDE_UP_DOWN);
    _stack.set_transition_duration(kTransitionDurationMs);

    _statusLabel.add_css_class("status-message");
    _statusLabel.set_halign(Gtk::Align::END);

    _stack.add(_selectionInfo.widget(), "info");
    _stack.add(_statusLabel, "status");
    _stack.set_visible_child("info");

    _notificationPostedSub = _notificationService.onPosted(
      [this](auto)
      {
        auto const feed = _notificationService.feed();

        if (!feed.entries.empty())
        {
          auto const& latest = feed.entries.back();
          auto const duration = latest.optTimeout.value_or(std::chrono::seconds{5});
          showMessage(latest.message, std::chrono::duration_cast<std::chrono::seconds>(duration));
        }
      });
  }

  StatusNotificationLabel::~StatusNotificationLabel() = default;

  void StatusNotificationLabel::showMessage(std::string_view message, std::chrono::seconds duration)
  {
    if (_timerConnection)
    {
      _timerConnection.disconnect();
    }

    _statusLabel.set_text(std::string{message});
    _stack.set_visible_child("status");

    _timerConnection = Glib::signal_timeout().connect_seconds(
      [this]
      {
        clearMessage();
        return false;
      },
      duration.count());
  }

  void StatusNotificationLabel::clearMessage()
  {
    if (_timerConnection)
    {
      _timerConnection.disconnect();
    }

    _statusLabel.set_text("");
    _stack.set_visible_child("info");
  }
} // namespace ao::gtk
