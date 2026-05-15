// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackListAdapter.h"

#include <runtime/StateTypes.h>

#include <gtkmm/entry.h>

#include <functional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class ViewService;
}

namespace ao::gtk
{
  class TrackFilterController final
  {
  public:
    using CreateSmartListSignal = sigc::signal<void(std::string)>;
    using StatusMessageFn = std::function<void(std::string_view)>;

    TrackFilterController(rt::ViewService& viewService, rt::ViewId viewId, Gtk::Entry& filterEntry);

    void setFilterExpression(std::string_view expression);
    void setStatusMessageCallback(const StatusMessageFn& callback);
    void setCreateSmartListSignal(CreateSmartListSignal* signal);

  private:
    void onFilterTextChanged();
    void onFilterDebounced();
    void onFilterStatusChanged(rt::FilterStatusChanged const& status);
    void updateFilterUi();

    rt::ViewService& _viewService;
    rt::ViewId _viewId;
    Gtk::Entry& _filterEntry;

    TrackFilterMode _filterMode = TrackFilterMode::None;
    std::string _filterExpression;
    bool _filterPending = false;
    bool _filterHasError = false;
    std::string _filterErrorMessage;

    sigc::scoped_connection _filterTextConnection;
    sigc::scoped_connection _filterIconConnection;
    sigc::scoped_connection _filterDebounceTimer;
    rt::Subscription _filterStatusSub;

    StatusMessageFn _statusMessageCallback;
    CreateSmartListSignal* _createSmartListSignal = nullptr;
  };
} // namespace ao::gtk
