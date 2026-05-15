// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackFilterController.h"
#include "track/TrackListAdapter.h"
#include <ao/utility/ScopedTimer.h>
#include <runtime/AppSession.h>
#include <runtime/CorePrimitives.h>
#include <runtime/ProjectionTypes.h>
#include <runtime/ViewService.h>

#include <gdkmm/enums.h>
#include <glibmm/main.h>
#include <glibmm/value.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <sigc++/functors/mem_fun.h>
#include <glib-object.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kFilterDebounceMs = 200;
  } // namespace

  TrackFilterController::TrackFilterController(rt::ViewService& viewService, rt::ViewId viewId, Gtk::Entry& filterEntry)
    : _viewService{viewService}, _viewId{viewId}, _filterEntry{filterEntry}
  {
    _filterTextConnection =
      _filterEntry.signal_changed().connect(sigc::mem_fun(*this, &TrackFilterController::onFilterTextChanged));

    _filterIconConnection = _filterEntry.signal_icon_press().connect(
      [this](Gtk::Entry::IconPosition iconPosition)
      {
        if (iconPosition != Gtk::Entry::IconPosition::SECONDARY)
        {
          return;
        }

        if (!_filterExpression.empty() && _createSmartListSignal)
        {
          _createSmartListSignal->emit(_filterExpression);
        }
      });

    auto const dropTarget = Gtk::DropTarget::create(Glib::Value<std::string>::value_type(), Gdk::DragAction::COPY);
    dropTarget->signal_drop().connect(
      [this](Glib::ValueBase const& value, double, double)
      {
        if (value.gobj()->g_type == G_TYPE_STRING)
        {
          auto val = Glib::Value<std::string>{};
          val.init(value.gobj());
          setFilterExpression(val.get());
          return true;
        }

        return false;
      },
      false);

    _filterEntry.add_controller(dropTarget);

    if (_viewId != rt::ViewId{})
    {
      _filterStatusSub =
        _viewService.onFilterStatusChanged(sigc::mem_fun(*this, &TrackFilterController::onFilterStatusChanged));
    }
  }

  void TrackFilterController::setFilterExpression(std::string_view expression)
  {
    _filterEntry.set_text(std::string{expression});
  }

  void TrackFilterController::setStatusMessageCallback(const StatusMessageFn& callback)
  {
    _statusMessageCallback = std::move(callback);
  }

  void TrackFilterController::setCreateSmartListSignal(CreateSmartListSignal* signal)
  {
    _createSmartListSignal = signal;
  }

  void TrackFilterController::onFilterTextChanged()
  {
    _filterDebounceTimer.disconnect();
    _filterDebounceTimer = Glib::signal_timeout().connect(
      [this]
      {
        onFilterDebounced();
        return false;
      },
      kFilterDebounceMs);
  }

  void TrackFilterController::onFilterDebounced()
  {
    auto const timer = utility::ScopedTimer{"TrackViewPage::onFilterDebounced"};
    auto const filterText = _filterEntry.get_text();

    if (_viewId == rt::ViewId{})
    {
      updateFilterUi();
      return;
    }

    auto const resolved = TrackListAdapter::resolveFilterExpression(filterText.raw());

    _filterMode = resolved.mode;
    _filterExpression = resolved.expression;
    _filterPending = true;

    if (resolved.mode == TrackFilterMode::None)
    {
      _viewService.setFilter(_viewId, "");
    }
    else
    {
      _viewService.setFilter(_viewId, resolved.expression);
    }

    updateFilterUi();
  }

  void TrackFilterController::onFilterStatusChanged(rt::FilterStatusChanged const& status)
  {
    if (status.viewId != _viewId)
    {
      return;
    }

    _filterPending = status.pending;
    _filterHasError = status.hasError;
    _filterErrorMessage = status.errorMessage;

    updateFilterUi();
  }

  void TrackFilterController::updateFilterUi()
  {
    if (_filterHasError)
    {
      _filterEntry.add_css_class("error");

      if (_statusMessageCallback)
      {
        _statusMessageCallback(std::format("Expression error: {}", _filterErrorMessage));
      }
    }
    else
    {
      _filterEntry.remove_css_class("error");

      if (_statusMessageCallback)
      {
        _statusMessageCallback("");
      }
    }

    auto const canCreateSmartList = !_filterExpression.empty() && !_filterPending && !_filterHasError;
    _filterEntry.set_icon_sensitive(Gtk::Entry::IconPosition::SECONDARY, canCreateSmartList);
  }
} // namespace ao::gtk
