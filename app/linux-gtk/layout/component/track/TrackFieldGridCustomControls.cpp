// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFieldGridCustomControls.h"

#include "layout/component/track/TrackFieldGridRows.h"
#include "sigc++/signal.h"
#include <ao/Type.h>

#include <glibmm/main.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::gtk::layout::track_field_grid
{
  namespace
  {
    constexpr auto kUndoTimeoutMs = 5000;

    std::string trimmedCopy(std::string_view text)
    {
      auto const first = text.find_first_not_of(" \t\n\r\f\v");

      if (first == std::string_view::npos)
      {
        return {};
      }

      auto const last = text.find_last_not_of(" \t\n\r\f\v");
      return std::string{text.substr(first, last - first + 1)};
    }
  } // namespace

  CustomPropertyUndoBar::CustomPropertyUndoBar()
  {
    _bar.set_orientation(Gtk::Orientation::HORIZONTAL);
    _bar.set_spacing(8);
    _bar.set_margin(8);
    _bar.add_css_class("ao-undo-bar");
    _bar.set_visible(false);

    _label.set_halign(Gtk::Align::START);
    _label.set_hexpand(true);
    _bar.append(_label);

    _undoButton.add_css_class("flat");
    _undoButton.add_css_class("ao-undo-button");
    _undoButton.signal_clicked().connect([this] { _undoRequested.emit(); });
    _bar.append(_undoButton);
  }

  CustomPropertyUndoBar::~CustomPropertyUndoBar()
  {
    if (_timerConn)
    {
      _timerConn.disconnect();
    }
  }

  Gtk::Widget& CustomPropertyUndoBar::widget()
  {
    return _bar;
  }

  void CustomPropertyUndoBar::show(std::string key, std::vector<TrackId> trackIds, std::string value)
  {
    _optPendingUndo = UndoState{.key = std::move(key), .trackIds = std::move(trackIds), .value = std::move(value)};
    _label.set_text(std::format("Property '{}' removed", _optPendingUndo->key));
    _bar.set_visible(true);

    if (_timerConn)
    {
      _timerConn.disconnect();
    }

    _timerConn = Glib::signal_timeout().connect(
      [this]
      {
        clear();
        return false;
      },
      kUndoTimeoutMs);
  }

  std::optional<UndoState> CustomPropertyUndoBar::takePendingUndo()
  {
    if (!_optPendingUndo)
    {
      return std::nullopt;
    }

    auto optPendingUndo = std::move(_optPendingUndo);
    clear();
    return optPendingUndo;
  }

  sigc::signal<void()>& CustomPropertyUndoBar::signalUndoRequested()
  {
    return _undoRequested;
  }

  void CustomPropertyUndoBar::clear()
  {
    _bar.set_visible(false);
    _optPendingUndo.reset();

    if (_timerConn)
    {
      _timerConn.disconnect();
    }
  }

  AddCustomPropertyRow::AddCustomPropertyRow(std::int32_t const actionSpacing)
    : _valueBox{_valueEntry, _submitButton, actionSpacing}
  {
    _keySlot.set_halign(Gtk::Align::FILL);
    _keySlot.set_hexpand(false);

    _keyEntry.set_placeholder_text("New Property");
    _keyEntry.set_width_chars(1);
    _keyEntry.set_max_width_chars(1);
    _keyEntry.add_css_class("ao-p-none");
    _keyEntry.set_has_frame(false);

    _valueEntry.set_placeholder_text("Value");
    _valueEntry.add_css_class("ao-p-none");
    _valueEntry.set_has_frame(false);
    _valueEntry.set_hexpand(true);

    _submitButton.set_icon_name("list-add-symbolic");
    _submitButton.set_halign(Gtk::Align::END);
    _submitButton.set_has_frame(false);
    _submitButton.add_css_class("ao-icon-button");
    _submitButton.set_tooltip_text("Add Custom Property");

    _submitButton.signal_clicked().connect([this] { onAddRequested(); });
    _keyEntry.signal_activate().connect([this] { onAddRequested(); });
    _valueEntry.signal_activate().connect([this] { onAddRequested(); });

    _keyEntry.property_text().signal_changed().connect([this] { _keyEntry.remove_css_class("error"); });
    _valueEntry.property_text().signal_changed().connect([this] { _valueEntry.remove_css_class("error"); });
  }

  FixedHeightWidgetSlot& AddCustomPropertyRow::keySlot()
  {
    return _keySlot;
  }

  FixedHeightWidgetSlot& AddCustomPropertyRow::valueSlot()
  {
    return _valueSlot;
  }

  void AddCustomPropertyRow::markKeyError()
  {
    _keyEntry.add_css_class("error");
    _keyEntry.error_bell();
  }

  void AddCustomPropertyRow::markValueError()
  {
    _valueEntry.add_css_class("error");
    _valueEntry.error_bell();
  }

  void AddCustomPropertyRow::clearInputs()
  {
    _keyEntry.set_text("");
    _valueEntry.set_text("");
  }

  sigc::signal<void(std::string, std::string)>& AddCustomPropertyRow::signalAddRequested()
  {
    return _addRequested;
  }

  void AddCustomPropertyRow::onAddRequested()
  {
    auto const keyText = _keyEntry.get_text();
    auto const valueText = _valueEntry.get_text();

    auto key = trimmedCopy(keyText.raw());
    auto value = std::string{valueText.raw()};

    auto hasError = false;

    if (key.empty())
    {
      markKeyError();
      hasError = true;
    }

    if (value.empty())
    {
      markValueError();
      hasError = true;
    }

    if (hasError)
    {
      return;
    }

    _addRequested.emit(std::move(key), std::move(value));
  }
} // namespace ao::gtk::layout::track_field_grid
