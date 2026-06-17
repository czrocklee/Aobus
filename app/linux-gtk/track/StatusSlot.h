// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/SelectionInfoLabel.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/uimodel/status/StatusSlotModel.h>

#include <glibmm/main.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <chrono>
#include <cstddef>

namespace ao::rt
{
  class NotificationService;
  class ViewService;
}

namespace ao::gtk
{
  /**
   * @brief StatusSlot provides a consolidated status view.
   *
   * It merges library task progress, notifications, and selection info into
   * a single component with priority-based display:
   * 1. Library Task Progress (Highest)
   * 2. Notifications
   * 3. Selection Info (Default / Idle)
   */
  class StatusSlot final
  {
  public:
    StatusSlot(rt::LibraryMutationService& mutation, rt::NotificationService& notifications, rt::ViewService& views);
    ~StatusSlot();

    StatusSlot(StatusSlot const&) = delete;
    StatusSlot& operator=(StatusSlot const&) = delete;
    StatusSlot(StatusSlot&&) = delete;
    StatusSlot& operator=(StatusSlot&&) = delete;

    Gtk::Widget& widget() { return _box; }

  private:
    void setupUi();

    void onLibraryTaskProgress(rt::LibraryMutationService::LibraryTaskProgressUpdated const& ev);
    void onLibraryTaskCompleted(std::size_t count);
    void onNotificationPosted(rt::NotificationId id);

    void renderState(uimodel::status::StatusSlotViewState const& state);

    void clearSeverityClasses();
    void startAutoDismissTimer(std::chrono::milliseconds timeout);

    rt::LibraryMutationService& _mutation;
    rt::NotificationService& _notifications;

    Gtk::Box _box;
    Gtk::Label _messageLabel;
    Gtk::ProgressBar _progressBar;
    SelectionInfoLabel _selectionInfo;

    uimodel::status::StatusSlotModel _model;
    sigc::connection _autoDismissTimer;

    rt::Subscription _progressSub;
    rt::Subscription _completedSub;
    rt::Subscription _notificationSub;

    static constexpr int kMaxMessageChars = 30;
    static constexpr int kProgressBarWidth = 150;
  };
} // namespace ao::gtk
