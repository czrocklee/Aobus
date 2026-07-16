// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "completion/EntryCompletionController.h"
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/track/TrackFilterCompleter.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>

#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <sigc++/connection.h>
#include <sigc++/functors/slot.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    using DebounceScheduler = std::function<sigc::connection(std::chrono::milliseconds, sigc::slot<bool()>)>;

    explicit TrackQuickFilter(rt::AppRuntime& runtime, DebounceScheduler debounceScheduler = {});
    ~TrackQuickFilter() override;

    TrackQuickFilter(TrackQuickFilter const&) = delete;
    TrackQuickFilter& operator=(TrackQuickFilter const&) = delete;
    TrackQuickFilter(TrackQuickFilter&&) = delete;
    TrackQuickFilter& operator=(TrackQuickFilter&&) = delete;

    CreateSmartListSignal& signalCreateSmartListRequested() { return _signalCreateSmartListRequested; }

    Gtk::Entry& entry() noexcept { return _entry; }
    Gtk::Entry const& entry() const noexcept { return _entry; }

    void setText(Glib::ustring const& text);
    Glib::ustring text() const;
    void setPosition(std::int32_t position);
    std::int32_t position() const;

  private:
    std::optional<rt::CompletionResult> complete(std::string_view text, std::size_t cursor);

    void handleFilterTextChanged();
    void handleClearClicked();
    void handleCreateSmartListClicked();
    void applyState(uimodel::TrackFilterViewState const& view);
    void updateClearButton();
    void setActive(bool active);

    rt::AppRuntime& _runtime;
    Gtk::Entry _entry;
    Gtk::Button _clearButton;
    Gtk::Button _createSmartListButton;
    Glib::RefPtr<Gtk::EventControllerFocus> _focusControllerPtr;
    std::string _resolvedExpression;
    uimodel::TrackFilterCompleter _completer;
    EntryCompletionController _completionController;
    DebounceScheduler _debounceScheduler;

    sigc::scoped_connection _textChangedConn;
    sigc::scoped_connection _debounceTimer;

    CreateSmartListSignal _signalCreateSmartListRequested;
    uimodel::TrackFilterViewModel _filterViewModel;
  };
} // namespace ao::gtk
