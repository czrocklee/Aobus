// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "ao/Error.h"
#include "runtime/CorePrimitives.h"
#include "runtime/ProjectionTypes.h"

#include <gtkmm/entry.h>
#include <sigc++/scoped_connection.h>
#include <sigc++/signal.h>

#include <optional>
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
    void onFilterDebounced();
    void onFilterStatusChanged(rt::FilterStatusChanged const& status);
    void onFocusedViewChanged(rt::ViewId viewId);
    void updateUi();

    rt::AppRuntime& _runtime;
    rt::ViewId _viewId{};

    std::string _filterExpression;
    bool _filterPending = false;
    std::optional<ao::Error> _optFilterError;

    sigc::scoped_connection _textChangedConn;
    sigc::scoped_connection _debounceTimer;
    rt::Subscription _filterStatusSub;
    rt::Subscription _focusSub;

    CreateSmartListSignal _signalCreateSmartListRequested;
  };
} // namespace ao::gtk
