// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>

#include <glibmm/refptr.h>

#include <functional>
#include <memory>
#include <string>

namespace Gtk
{
  class ColumnView;
  class ColumnViewColumn;
  class ScrolledWindow;
}

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class TrackSelectionController;

  /**
   * Owns one ColumnView generation's internal row-order drag surface.
   */
  class TrackOrderDragController final
  {
  public:
    struct Callbacks final
    {
      std::function<void(std::string)> onStatus;
      std::function<void()> onClearStatus;
    };

    TrackOrderDragController(rt::AppRuntime& runtime,
                             rt::ViewId viewId,
                             i18n::MessageCatalog const& textCatalog,
                             Gtk::ColumnView& columnView,
                             Gtk::ScrolledWindow& scrolledWindow,
                             TrackSelectionController& selectionController,
                             Callbacks callbacks);
    ~TrackOrderDragController();

    TrackOrderDragController(TrackOrderDragController const&) = delete;
    TrackOrderDragController& operator=(TrackOrderDragController const&) = delete;
    TrackOrderDragController(TrackOrderDragController&&) = delete;
    TrackOrderDragController& operator=(TrackOrderDragController&&) = delete;

    Glib::RefPtr<Gtk::ColumnViewColumn> const& column() const noexcept { return _columnPtr; }

  private:
    struct State;
    static Glib::RefPtr<Gtk::ColumnViewColumn> makeColumn(std::shared_ptr<State> const& statePtr);

    std::shared_ptr<State> _statePtr;
    Glib::RefPtr<Gtk::ColumnViewColumn> _columnPtr;
  };
} // namespace ao::gtk
