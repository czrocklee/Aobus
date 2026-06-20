// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "completion/EntryCompletionController.h"
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>
#include <ao/uimodel/track/TrackFilterViewModel.h>

#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  /**
   * @brief TrackQuickFilter is a global entry control that manages filtering for the focused track view.
   */
  class TrackQuickFilter final : public Gtk::Box
  {
  public:
    using CreateSmartListSignal = sigc::signal<void(std::string)>;

    explicit TrackQuickFilter(rt::AppRuntime& runtime);
    ~TrackQuickFilter() override;

    TrackQuickFilter(TrackQuickFilter const&) = delete;
    TrackQuickFilter& operator=(TrackQuickFilter const&) = delete;
    TrackQuickFilter(TrackQuickFilter&&) = delete;
    TrackQuickFilter& operator=(TrackQuickFilter&&) = delete;

    CreateSmartListSignal& signalCreateSmartListRequested() { return _signalCreateSmartListRequested; }

    Gtk::Entry& entry() noexcept { return _entry; }
    Gtk::Entry const& entry() const noexcept { return _entry; }

    void setText(Glib::ustring const& text);
    Glib::ustring getText() const;
    void setPosition(std::int32_t position);
    std::int32_t getPosition() const;
    void activate();

  private:
    std::optional<rt::CompletionResult> complete(std::string_view text, std::size_t cursor);

    void onFilterTextChanged();
    void onClearClicked();
    void onCreateSmartListClicked();
    void applyState(uimodel::track::TrackFilterViewState const& view);
    void updateClearButton();
    void setActive(bool active);

    rt::AppRuntime& _runtime;
    Gtk::Entry _entry;
    Gtk::Button _clearButton;
    Gtk::Button _createSmartListButton;
    Glib::RefPtr<Gtk::EventControllerFocus> _focusControllerPtr;
    std::string _resolvedExpression;
    rt::QueryExpressionCompleter _completer;
    EntryCompletionController _completionController;

    sigc::scoped_connection _textChangedConn;
    sigc::scoped_connection _debounceTimer;

    CreateSmartListSignal _signalCreateSmartListRequested;
    uimodel::track::TrackFilterViewModel _controller;
  };
} // namespace ao::gtk
