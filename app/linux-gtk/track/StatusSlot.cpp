// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/StatusSlot.h"

#include "layout/LayoutConstants.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/status/StatusSlotModel.h>

#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ao::gtk
{
  StatusSlot::StatusSlot(rt::LibraryChanges const& changes,
                         rt::NotificationService& notifications,
                         rt::ViewService& views)
    : _changes{changes}, _notifications{notifications}, _selectionInfo{views}
  {
    setupUi();

    _progressSub = _changes.onLibraryTaskProgress([this](auto const& ev) { onLibraryTaskProgress(ev); });
    _completedSub = _changes.onLibraryTaskCompleted([this](auto count) { onLibraryTaskCompleted(count); });
    _notificationSub = _notifications.onPosted([this](auto id) { onNotificationPosted(id); });

    renderState(_model.initialState());
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

  void StatusSlot::onLibraryTaskProgress(rt::LibraryChanges::LibraryTaskProgressUpdated const& ev)
  {
    renderState(_model.onLibraryTaskProgress(ev.message, ev.fraction));
  }

  void StatusSlot::onLibraryTaskCompleted(std::size_t count)
  {
    renderState(_model.onLibraryTaskCompleted(count));
  }

  void StatusSlot::onNotificationPosted(rt::NotificationId id)
  {
    auto const feed = _notifications.feed();
    auto const iter = std::ranges::find(feed.entries, id, &rt::NotificationEntry::id);

    if (iter == feed.entries.end())
    {
      return;
    }

    if (auto const optState = _model.onNotificationPosted(*iter); optState)
    {
      renderState(*optState);
    }
  }

  void StatusSlot::renderState(uimodel::status::StatusSlotViewState const& state)
  {
    if (_autoDismissTimer)
    {
      _autoDismissTimer.disconnect();
    }

    clearSeverityClasses();

    switch (state.mode)
    {
      case uimodel::status::StatusSlotDisplayMode::SelectionInfo:
        _messageLabel.set_visible(false);
        _progressBar.set_visible(false);
        _selectionInfo.widget().set_visible(true);
        return;

      case uimodel::status::StatusSlotDisplayMode::Progress:
        _selectionInfo.widget().set_visible(false);
        _messageLabel.set_visible(true);
        _progressBar.set_visible(true);
        _messageLabel.set_text(state.message);
        _progressBar.set_fraction(state.progressFraction);
        return;

      case uimodel::status::StatusSlotDisplayMode::Message:
        _selectionInfo.widget().set_visible(false);
        _messageLabel.set_visible(true);
        _progressBar.set_visible(false);
        _messageLabel.set_text(state.message);

        if (state.optSeverity)
        {
          _messageLabel.add_css_class(std::string{uimodel::status::statusSlotSeverityCssClass(*state.optSeverity)});
        }

        if (state.optAutoDismissTimeout)
        {
          startAutoDismissTimer(*state.optAutoDismissTimeout);
        }

        return;
    }
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
        renderState(_model.onAutoDismiss());
        return false;
      },
      static_cast<std::uint32_t>(timeout.count()));
  }

  void StatusSlot::clearSeverityClasses()
  {
    _messageLabel.remove_css_class("ao-status-info");
    _messageLabel.remove_css_class("ao-status-warning");
    _messageLabel.remove_css_class("ao-status-error");
  }
} // namespace ao::gtk
