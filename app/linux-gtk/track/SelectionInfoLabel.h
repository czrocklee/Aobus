// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <chrono>
#include <cstddef>
#include <optional>

namespace ao::rt
{
  class LibraryChanges;
  struct LibraryChangeSet;
  class ViewService;
  class WorkspaceService;
}

namespace ao::gtk
{
  /**
   * SelectionInfoLabel displays "N items selected" with optional total duration.
   */
  class SelectionInfoLabel final
  {
  public:
    SelectionInfoLabel(rt::ViewService& viewService,
                       rt::WorkspaceService& workspace,
                       rt::LibraryChanges const& changes,
                       i18n::MessageCatalog textCatalog);
    ~SelectionInfoLabel();

    // Not copyable or movable
    SelectionInfoLabel(SelectionInfoLabel const&) = delete;
    SelectionInfoLabel& operator=(SelectionInfoLabel const&) = delete;
    SelectionInfoLabel(SelectionInfoLabel&&) = delete;
    SelectionInfoLabel& operator=(SelectionInfoLabel&&) = delete;

    Gtk::Widget& widget() { return _label; }

  private:
    void refreshActiveView(rt::ViewId activeViewId, rt::LibraryChangeSet const* changes = nullptr);
    void updateState(std::size_t count, std::optional<std::chrono::milliseconds> optTotalDuration = std::nullopt);

    rt::ViewService& _viewService;
    rt::WorkspaceService& _workspace;
    i18n::MessageCatalog _textCatalog;
    Gtk::Label _label{};
    rt::ViewId _activeViewId{rt::kInvalidViewId};
    async::Subscription _selectionChangedSub;
    async::Subscription _workspaceChangedSub;
    async::Subscription _libraryChangedSub;
  };
} // namespace ao::gtk
