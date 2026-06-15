// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "common/DismissController.h"
#include "layout/LayoutConstants.h"
#include <ao/rt/CompletionResult.h>

#include <glibmm/object.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/listview.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>
#include <sigc++/scoped_connection.h>

#include <cstddef>
#include <cstdint>

namespace Gio
{
  template<typename T>
  class ListStore;
}

namespace ao::gtk
{
  struct EntryCompletionControllerOptions final
  {
    bool suppressPopup = false;
    std::int32_t popoverWidth = layout::kCompletionPopoverWidth;
    std::int32_t popoverMaxHeight = layout::kCompletionPopoverMaxHeight;
  };

  class EntryCompletionController final
  {
  public:
    EntryCompletionController(Gtk::Entry& entry,
                              rt::CompletionProvider provider,
                              EntryCompletionControllerOptions options = {});
    ~EntryCompletionController();

    EntryCompletionController(EntryCompletionController const&) = delete;
    EntryCompletionController& operator=(EntryCompletionController const&) = delete;
    EntryCompletionController(EntryCompletionController&&) = delete;
    EntryCompletionController& operator=(EntryCompletionController&&) = delete;

    void update();
    void hide();
    void applySelected();
    bool moveSelection(std::int32_t delta);

    void setTextProgrammatically(Glib::ustring const& text);

  private:
    bool handleKeyPressed(std::uint32_t keyval);
    void clearCompletionState();

    Gtk::Entry& _entry;
    rt::CompletionProvider _provider;
    EntryCompletionControllerOptions _options;
    Gtk::Popover _popover;
    Gtk::ScrolledWindow _scrolledWindow;
    Gtk::ListView _listView;
    sigc::scoped_connection _changedConnection;
    // Owned by _entry once installed; kept here so the destructor can remove them, otherwise their
    // lambdas would outlive `this` on a borrowed entry that survives this controller (e.g. when a
    // host re-installs a fresh provider on the same entry) and fire into freed memory.
    Glib::RefPtr<Gtk::EventControllerKey> _keyControllerPtr;
    Glib::RefPtr<Gtk::EventControllerKey> _popoverKeyControllerPtr;
    Glib::RefPtr<Gtk::GestureClick> _clickControllerPtr;
    Glib::RefPtr<Gtk::EventControllerFocus> _focusControllerPtr;
    DismissController _dismissController;
    Glib::RefPtr<Gio::ListStore<Glib::Object>> _itemsPtr;
    Glib::RefPtr<Gtk::SingleSelection> _selectionPtr;
    std::size_t _replaceBegin = 0;
    std::size_t _replaceEnd = 0;
    bool _hasReplacement = false;
  };
} // namespace ao::gtk
