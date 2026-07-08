// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackQuickFilter.h"

#include "completion/EntryCompletionController.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>

#include <gdkmm/enums.h>
#include <glibmm/main.h>
#include <glibmm/value.h>
#include <gobject/gtype.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <sigc++/connection.h>
#include <sigc++/functors/mem_fun.h>
#include <sigc++/functors/slot.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    constexpr auto kFilterDebounceInterval = std::chrono::milliseconds{200};

    sigc::connection scheduleDefaultDebounce(std::chrono::milliseconds interval, sigc::slot<bool()> callback)
    {
      return Glib::signal_timeout().connect(std::move(callback), interval.count());
    }

    std::optional<std::string> stringFromDropValue(Glib::ValueBase const& value)
    {
      if (value.gobj()->g_type != G_TYPE_STRING)
      {
        return std::nullopt;
      }

      auto stringValue = Glib::Value<std::string>{};
      stringValue.init(value.gobj());
      return stringValue.get();
    }
  } // namespace

  TrackQuickFilter::TrackQuickFilter(rt::AppRuntime& runtime, DebounceScheduler debounceScheduler)
    : Gtk::Box{Gtk::Orientation::HORIZONTAL, 0}
    , _runtime{runtime}
    , _completer{_runtime.completion()}
    , _completionController{_entry,
                            [this](std::string_view text, std::size_t cursor) { return complete(text, cursor); }}
    , _debounceScheduler{std::move(debounceScheduler)}
    , _textChangedConn{_entry.signal_changed().connect(sigc::mem_fun(*this, &TrackQuickFilter::onFilterTextChanged))}
    , _controller{_runtime.views(),
                  _runtime.workspace(),
                  [this](ao::uimodel::TrackFilterViewState const& state) { applyState(state); }}
  {
    add_css_class("ao-quick-filter");
    set_hexpand(true);

    _entry.add_css_class("ao-quick-filter-entry");
    _entry.set_placeholder_text("Search songs, artists, albums, tags...");
    _entry.set_hexpand(true);
    _entry.set_icon_from_icon_name("system-search-symbolic", Gtk::Entry::IconPosition::PRIMARY);
    _entry.set_icon_sensitive(Gtk::Entry::IconPosition::PRIMARY, false);

    _clearButton.add_css_class("ao-quick-filter-action");
    _clearButton.add_css_class("ao-quick-filter-clear");
    _clearButton.set_icon_name("edit-clear-symbolic");
    _clearButton.set_has_frame(false);
    _clearButton.set_tooltip_text("Clear filter");
    _clearButton.set_visible(false);
    _clearButton.signal_clicked().connect(sigc::mem_fun(*this, &TrackQuickFilter::onClearClicked));

    _createSmartListButton.add_css_class("ao-quick-filter-action");
    _createSmartListButton.add_css_class("ao-quick-filter-create");
    _createSmartListButton.set_icon_name("list-add-symbolic");
    _createSmartListButton.set_has_frame(false);
    _createSmartListButton.set_tooltip_text("Create smart list from current filter");
    _createSmartListButton.set_sensitive(false);
    _createSmartListButton.signal_clicked().connect(sigc::mem_fun(*this, &TrackQuickFilter::onCreateSmartListClicked));

    append(_entry);
    append(_clearButton);
    append(_createSmartListButton);

    _focusControllerPtr = Gtk::EventControllerFocus::create();
    _focusControllerPtr->signal_enter().connect([this] { setActive(true); });
    _focusControllerPtr->signal_leave().connect([this] { setActive(false); });
    add_controller(_focusControllerPtr);

    auto const dropTargetPtr = Gtk::DropTarget::create(Glib::Value<std::string>::value_type(), Gdk::DragAction::COPY);
    dropTargetPtr->signal_drop().connect(
      [this](Glib::ValueBase const& value, double /*x*/, double /*y*/)
      {
        if (auto const optText = stringFromDropValue(value); optText)
        {
          _entry.set_text(*optText);
          return true;
        }

        return false;
      },
      false);

    _entry.add_controller(dropTargetPtr);
  }

  TrackQuickFilter::~TrackQuickFilter() = default;

  void TrackQuickFilter::setText(Glib::ustring const& text)
  {
    _entry.set_text(text);
  }

  Glib::ustring TrackQuickFilter::text() const
  {
    return _entry.get_text();
  }

  void TrackQuickFilter::setPosition(std::int32_t position)
  {
    _entry.set_position(position);
  }

  std::int32_t TrackQuickFilter::position() const
  {
    return _entry.get_position();
  }

  void TrackQuickFilter::activate()
  {
    _entry.activate();
  }

  std::optional<rt::CompletionResult> TrackQuickFilter::complete(std::string_view text, std::size_t cursor)
  {
    return _completer.complete(text, cursor);
  }

  void TrackQuickFilter::onFilterTextChanged()
  {
    updateClearButton();
    _debounceTimer.disconnect();
    auto callback = sigc::slot<bool()>{[this]
                                       {
                                         _controller.updateFilter(_entry.get_text().raw());
                                         return false;
                                       }};

    _debounceTimer = _debounceScheduler ? _debounceScheduler(kFilterDebounceInterval, std::move(callback))
                                        : scheduleDefaultDebounce(kFilterDebounceInterval, std::move(callback));
  }

  void TrackQuickFilter::onClearClicked()
  {
    _entry.set_text({});
    _entry.grab_focus();
  }

  void TrackQuickFilter::onCreateSmartListClicked()
  {
    if (!_resolvedExpression.empty())
    {
      _signalCreateSmartListRequested.emit(_resolvedExpression);
    }
  }

  void TrackQuickFilter::applyState(ao::uimodel::TrackFilterViewState const& view)
  {
    set_sensitive(view.enabled);
    _resolvedExpression = view.resolvedExpression;

    if (_entry.get_text().raw() != view.entryText)
    {
      _textChangedConn.block();
      _completionController.setTextProgrammatically(view.entryText);
      _textChangedConn.unblock();
    }

    if (view.hasError)
    {
      add_css_class("ao-query-invalid");
      _entry.add_css_class("ao-query-invalid");
    }
    else
    {
      remove_css_class("ao-query-invalid");
      _entry.remove_css_class("ao-query-invalid");
    }

    set_tooltip_text(view.tooltip);
    _entry.set_tooltip_text(view.tooltip);
    _createSmartListButton.set_sensitive(view.canCreateSmartList);
    updateClearButton();
  }

  void TrackQuickFilter::updateClearButton()
  {
    _clearButton.set_visible(!_entry.get_text().empty());
  }

  void TrackQuickFilter::setActive(bool active)
  {
    if (active)
    {
      add_css_class("ao-quick-filter-active");
    }
    else
    {
      remove_css_class("ao-quick-filter-active");
    }
  }
} // namespace ao::gtk
