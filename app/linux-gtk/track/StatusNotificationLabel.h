// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/SelectionInfoLabel.h"
#include <runtime/CorePrimitives.h>

#include <gtkmm/label.h>
#include <gtkmm/stack.h>

#include <chrono>
#include <memory>

namespace ao::rt
{
  class NotificationService;
  class ViewService;
}

namespace ao::gtk
{
  /**
   * StatusNotificationLabel manages a Gtk::Stack that toggles between
   * a SelectionInfoLabel and transient notification messages.
   */
  class StatusNotificationLabel final
  {
  public:
    explicit StatusNotificationLabel(rt::NotificationService& notificationService, rt::ViewService& viewService);
    ~StatusNotificationLabel();

    Gtk::Widget& widget() { return _stack; }

  private:
    void showMessage(std::string_view message, std::chrono::seconds duration);
    void clearMessage();

    rt::NotificationService& _notificationService;
    Gtk::Stack _stack;
    SelectionInfoLabel _selectionInfo;
    Gtk::Label _statusLabel;

    sigc::connection _timerConnection;
    rt::Subscription _notificationPostedSub;

    static constexpr int kTransitionDurationMs = 250;
  };
} // namespace ao::gtk
