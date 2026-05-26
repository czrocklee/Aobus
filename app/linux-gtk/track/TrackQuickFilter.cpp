// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/track/TrackFilterViewModel.h>

#include <gdkmm/enums.h>
#include <glibmm/main.h>
#include <glibmm/value.h>
#include <gobject/gtype.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <sigc++/functors/mem_fun.h>

#include <memory>
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
        if (iconPosition == Gtk::Entry::IconPosition::SECONDARY && !_resolvedExpression.empty())
        {
          _signalCreateSmartListRequested.emit(_resolvedExpression);
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

    _controller = std::make_unique<ao::uimodel::track::TrackFilterViewModel>(
      _runtime.views(),
      _runtime.workspace(),
      [this](ao::uimodel::track::TrackFilterViewState const& state) { applyState(state); });
  }

  TrackQuickFilter::~TrackQuickFilter() = default;

  void TrackQuickFilter::onFilterTextChanged()
  {
    _debounceTimer.disconnect();
    _debounceTimer = Glib::signal_timeout().connect(
      [this]
      {
        _controller->updateFilter(get_text().raw());
        return false;
      },
      kFilterDebounceMs);
  }

  void TrackQuickFilter::applyState(ao::uimodel::track::TrackFilterViewState const& view)
  {
    set_sensitive(view.enabled);
    _resolvedExpression = view.resolvedExpression;

    if (get_text().raw() != view.entryText)
    {
      _textChangedConn.block();
      set_text(view.entryText);
      _textChangedConn.unblock();
    }

    if (view.hasError)
    {
      add_css_class("error");
    }
    else
    {
      remove_css_class("error");
    }

    set_tooltip_text(view.tooltip);
    set_icon_sensitive(Gtk::Entry::IconPosition::SECONDARY, view.canCreateSmartList);
  }
} // namespace ao::gtk
