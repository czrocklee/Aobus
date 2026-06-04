// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/component/track/TrackFieldGridRows.h"
#include "layout/component/track/TrackFieldGridWidgets.h"
#include "sigc++/connection.h"
#include <ao/Type.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::layout::track_field_grid
{
  class CustomPropertyUndoBar final
  {
  public:
    CustomPropertyUndoBar();
    ~CustomPropertyUndoBar();

    CustomPropertyUndoBar(CustomPropertyUndoBar const&) = delete;
    CustomPropertyUndoBar& operator=(CustomPropertyUndoBar const&) = delete;
    CustomPropertyUndoBar(CustomPropertyUndoBar&&) = delete;
    CustomPropertyUndoBar& operator=(CustomPropertyUndoBar&&) = delete;

    Gtk::Widget& widget();

    void show(std::string key, std::vector<TrackId> trackIds, std::string value);
    std::optional<UndoState> takePendingUndo();

    sigc::signal<void()>& signalUndoRequested();

  private:
    void clear();

    Gtk::Box _bar{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Label _label{};
    Gtk::Button _undoButton{"Undo"};
    std::optional<UndoState> _optPendingUndo;
    sigc::signal<void()> _undoRequested;
    sigc::connection _timerConn;
  };

  class AddCustomPropertyRow final
  {
  public:
    explicit AddCustomPropertyRow(std::int32_t actionSpacing);
    ~AddCustomPropertyRow() = default;

    AddCustomPropertyRow(AddCustomPropertyRow const&) = delete;
    AddCustomPropertyRow& operator=(AddCustomPropertyRow const&) = delete;
    AddCustomPropertyRow(AddCustomPropertyRow&&) = delete;
    AddCustomPropertyRow& operator=(AddCustomPropertyRow&&) = delete;

    FixedHeightWidgetSlot& keySlot();
    FixedHeightWidgetSlot& valueSlot();

    void markKeyError();
    void markValueError();
    void clearInputs();

    sigc::signal<void(std::string, std::string)>& signalAddRequested();

  private:
    void onAddRequested();

    Gtk::Entry _keyEntry;
    FixedHeightWidgetSlot _keySlot{_keyEntry, false, false};

    Gtk::Entry _valueEntry;
    Gtk::Button _submitButton;
    FieldValueWrapper _valueClip;
    FixedHeightWidgetSlot _valueSlot{_valueClip, true};

    sigc::signal<void(std::string, std::string)> _addRequested;
  };
} // namespace ao::gtk::layout::track_field_grid
