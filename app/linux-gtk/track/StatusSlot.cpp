// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/StatusSlot.h"

#include "layout/LayoutConstants.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>

#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace ao::gtk
{
  StatusSlot::StatusSlot(rt::LibraryMutationService& mutation,
                         rt::NotificationService& notifications,
                         rt::ViewService& views)
    : _mutation{mutation}, _notifications{notifications}, _selectionInfo{views}
  {
    setupUi();

    _progressSub = _mutation.onLibraryTaskProgress([this](auto const& ev) { onLibraryTaskProgress(ev); });
    _completedSub = _mutation.onLibraryTaskCompleted([this](auto count) { onLibraryTaskCompleted(count); });
    _notificationSub = _notifications.onPosted([this](auto id) { onNotificationPosted(id); });

    showSelectionInfo();
  }

  StatusSlot::~StatusSlot()
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }
  }

  void StatusSlot::setupUi()
  {
    _box.set_orientation(Gtk::Orientation::HORIZONTAL);
    _box.set_spacing(layout::kSpacingSmall);

    _messageLabel.set_ellipsize(Pango::EllipsizeMode::END);
    _messageLabel.set_max_width_chars(kMaxMessageChars);

    _progressBar.set_size_request(kProgressBarWidth, -1);
    _progressBar.set_valign(Gtk::Align::CENTER);
    _progressBar.set_visible(false);

    _box.append(_selectionInfo.widget());
    _box.append(_messageLabel);
    _box.append(_progressBar);

    _messageLabel.set_visible(false);
  }

  void StatusSlot::onLibraryTaskProgress(rt::LibraryMutationService::LibraryTaskProgressUpdated const& ev)
  {
    _taskActive = true;

    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    showProgress(ev.message, ev.fraction);
  }

  void StatusSlot::onLibraryTaskCompleted(std::size_t count)
  {
    _taskActive = false;
    _progressBar.set_visible(false);

    if (_optDeferredNotification)
    {
      auto const entry = *_optDeferredNotification;
      _optDeferredNotification.reset();
      showNotification(entry);
    }
    else
    {
      if (auto const message =
            count == 0 ? "Library is up to date" : std::format("Scan complete: {} tracks added", count);
          !message.empty())
      {
        _messageLabel.set_text(message);
        clearSeverityClasses();
        _messageLabel.add_css_class("ao-status-info");

        startAutoDismissTimer(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultAutoDismissDuration));
      }
    }
  }

  void StatusSlot::onNotificationPosted(rt::NotificationId id)
  {
    auto const feed = _notifications.feed();
    auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);

    if (iter == feed.entries.end())
    {
      return;
    }

    if (_taskActive)
    {
      _optDeferredNotification = *iter;
    }
    else
    {
      showNotification(*iter);
    }
  }

  void StatusSlot::showProgress(std::string const& message, double fraction)
  {
    _selectionInfo.widget().set_visible(false);
    _messageLabel.set_visible(true);
    _progressBar.set_visible(true);

    _messageLabel.set_text(message);
    _progressBar.set_fraction(fraction);

    clearSeverityClasses();
  }

  void StatusSlot::showNotification(rt::NotificationEntry const& entry)
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    _selectionInfo.widget().set_visible(false);
    _messageLabel.set_visible(true);
    _progressBar.set_visible(false);

    _messageLabel.set_text(entry.message);

    clearSeverityClasses();

    switch (entry.severity)
    {
      case rt::NotificationSeverity::Info: _messageLabel.add_css_class("ao-status-info"); break;
      case rt::NotificationSeverity::Warning: _messageLabel.add_css_class("ao-status-warning"); break;
      case rt::NotificationSeverity::Error: _messageLabel.add_css_class("ao-status-error"); break;
    }

    if (!entry.sticky)
    {
      auto const timeout =
        entry.optTimeout.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultAutoDismissDuration));
      startAutoDismissTimer(std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
    }
  }

  void StatusSlot::showSelectionInfo()
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    _messageLabel.set_visible(false);
    _progressBar.set_visible(false);
    _selectionInfo.widget().set_visible(true);

    clearSeverityClasses();
  }

  void StatusSlot::clearSeverityClasses()
  {
    _messageLabel.remove_css_class("ao-status-info");
    _messageLabel.remove_css_class("ao-status-warning");
    _messageLabel.remove_css_class("ao-status-error");
  }

  void StatusSlot::startAutoDismissTimer(std::chrono::milliseconds timeout)
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    _autoDismissTimer = Glib::signal_timeout().connect(
      [this]
      {
        showSelectionInfo();
        return false;
      },
      static_cast<std::uint32_t>(timeout.count()));
  }
} // namespace ao::gtk
