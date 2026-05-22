// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "runtime/AppRuntime.h"
#include "runtime/CorePrimitives.h"
#include "runtime/ProjectionTypes.h"
#include "runtime/ViewService.h"
#include "runtime/WorkspaceService.h"
#include "track/TrackListAdapter.h"

#include <gdkmm/enums.h>
#include <glibmm/main.h>
#include <glibmm/value.h>
#include <gobject/gtype.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <sigc++/functors/mem_fun.h>

#include <format>
#include <string>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kFilterDebounceMs = 200;
  }

  TrackQuickFilter::TrackQuickFilter(rt::AppRuntime& runtime)
    : _runtime{runtime}
    , _textChangedConn{signal_changed().connect(sigc::mem_fun(*this, &TrackQuickFilter::onFilterTextChanged))}
    , _focusSub{
        _runtime.workspace().onFocusedViewChanged(sigc::mem_fun(*this, &TrackQuickFilter::onFocusedViewChanged))}
  {
    set_placeholder_text("Quick filter or expression...");
    set_hexpand(true);
    set_icon_from_icon_name("system-search-symbolic", Gtk::Entry::IconPosition::PRIMARY);
    set_icon_sensitive(Gtk::Entry::IconPosition::PRIMARY, false);
    set_icon_from_icon_name("list-add-symbolic", Gtk::Entry::IconPosition::SECONDARY);
    set_icon_activatable(true, Gtk::Entry::IconPosition::SECONDARY);
    set_icon_tooltip_text("Create smart list from current filter", Gtk::Entry::IconPosition::SECONDARY);

    signal_icon_press().connect(
      [this](Gtk::Entry::IconPosition iconPosition)
      {
        if (iconPosition == Gtk::Entry::IconPosition::SECONDARY && !_filterExpression.empty())
        {
          _signalCreateSmartListRequested.emit(_filterExpression);
        }
      });

    auto const dropTarget = Gtk::DropTarget::create(Glib::Value<std::string>::value_type(), Gdk::DragAction::COPY);
    dropTarget->signal_drop().connect(
      [this](Glib::ValueBase const& value, double /*x*/, double /*y*/)
      {
        if (value.gobj()->g_type == G_TYPE_STRING)
        {
          auto val = Glib::Value<std::string>{};
          val.init(value.gobj());
          set_text(val.get());
          return true;
        }

        return false;
      },
      false);

    add_controller(dropTarget);

    onFocusedViewChanged(_runtime.workspace().layoutState().activeViewId);
  }

  TrackQuickFilter::~TrackQuickFilter() = default;

  void TrackQuickFilter::onFocusedViewChanged(rt::ViewId viewId)
  {
    _viewId = viewId;
    _filterStatusSub.reset();

    if (_viewId == rt::kInvalidViewId)
    {
      set_sensitive(false);
      _filterExpression.clear();
      _filterPending = false;
      _optFilterError.reset();

      _textChangedConn.block();
      set_text("");
      _textChangedConn.unblock();

      updateUi();
      return;
    }

    set_sensitive(true);
    auto const state = _runtime.views().trackListState(_viewId);

    _textChangedConn.block();
    set_text(state.filterExpression);
    _textChangedConn.unblock();

    _filterStatusSub =
      _runtime.views().onFilterStatusChanged(sigc::mem_fun(*this, &TrackQuickFilter::onFilterStatusChanged));

    auto const resolved = TrackListAdapter::resolveFilterExpression(state.filterExpression);
    _filterExpression = resolved.expression;
    _filterPending = false;
    _optFilterError.reset();

    updateUi();
  }

  void TrackQuickFilter::onFilterTextChanged()
  {
    _debounceTimer.disconnect();
    _debounceTimer = Glib::signal_timeout().connect(
      [this]
      {
        onFilterDebounced();
        return false;
      },
      kFilterDebounceMs);
  }

  void TrackQuickFilter::onFilterDebounced()
  {
    auto const filterText = get_text();

    if (_viewId == rt::kInvalidViewId)
    {
      return;
    }

    auto const resolved = TrackListAdapter::resolveFilterExpression(filterText.raw());
    _filterExpression = resolved.expression;
    _filterPending = true;

    if (resolved.mode == TrackFilterMode::None)
    {
      _runtime.views().setFilter(_viewId, "");
    }
    else
    {
      _runtime.views().setFilter(_viewId, resolved.expression);
    }

    updateUi();
  }

  void TrackQuickFilter::onFilterStatusChanged(rt::FilterStatusChanged const& status)
  {
    if (status.viewId != _viewId)
    {
      return;
    }

    _filterPending = status.pending;
    _optFilterError = status.optError;

    updateUi();
  }

  void TrackQuickFilter::updateUi()
  {
    if (_optFilterError)
    {
      add_css_class("error");
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      set_tooltip_text(std::format("Expression error: {}", _optFilterError->message));
    }
    else
    {
      remove_css_class("error");
      set_tooltip_text("");
    }

    auto const canCreateSmartList = !_filterExpression.empty() && !_filterPending && !_optFilterError;
    set_icon_sensitive(Gtk::Entry::IconPosition::SECONDARY, canCreateSmartList);
  }
} // namespace ao::gtk
