// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/track/TrackFilterViewModel.h>

#include <gtkmm/entry.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <string>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  /**
   * @brief TrackQuickFilter is a global entry widget that manages filtering for the focused track view.
   */
  class TrackQuickFilter final : public Gtk::Entry
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

  private:
    void onFilterTextChanged();
    void applyState(uimodel::track::TrackFilterViewState const& view);

    rt::AppRuntime& _runtime;
    std::string _resolvedExpression;

    sigc::scoped_connection _textChangedConn;
    sigc::scoped_connection _debounceTimer;

    CreateSmartListSignal _signalCreateSmartListRequested;
    uimodel::track::TrackFilterViewModel _controller;
  };
} // namespace ao::gtk
