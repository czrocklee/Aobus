// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackListAdapter.h"

#include <ao/Type.h>
#include <runtime/StateTypes.h>

#include <gtkmm/entry.h>

#include <functional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class AppSession;
}

namespace ao::gtk
{
  class TrackFilterController final
  {
  public:
    using CreateSmartListSignal = sigc::signal<void(std::string)>;
    using StatusMessageFn = std::function<void(std::string_view)>;

    TrackFilterController(ao::rt::AppSession& session,
                          ao::rt::ViewId viewId,
                          Gtk::Entry& filterEntry);

    void setFilterExpression(std::string_view expression);
    void setStatusMessageCallback(StatusMessageFn callback);
    void setCreateSmartListSignal(CreateSmartListSignal* signal);

  private:
    void onFilterTextChanged();
    void onFilterDebounced();
    void onFilterStatusChanged(ao::rt::FilterStatusChanged const& status);
    void updateFilterUi();

    ao::rt::AppSession& _session;
    ao::rt::ViewId _viewId;
    Gtk::Entry& _filterEntry;

    TrackFilterMode _filterMode = TrackFilterMode::None;
    std::string _filterExpression;
    bool _filterPending = false;
    bool _filterHasError = false;
    std::string _filterErrorMessage;

    sigc::connection _filterTextConnection;
    sigc::connection _filterIconConnection;
    sigc::connection _filterDebounceTimer;
    ao::rt::Subscription _filterStatusSub;

    StatusMessageFn _statusMessageCallback;
    CreateSmartListSignal* _createSmartListSignal = nullptr;
  };
} // namespace ao::gtk
