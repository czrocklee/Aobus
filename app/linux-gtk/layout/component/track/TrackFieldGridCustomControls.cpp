// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFieldGridCustomControls.h"

#include <gtkmm/enums.h>
#include <gtkmm/popover.h>
#include <sigc++/signal.h>

#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout::track_field_grid
{
  namespace
  {
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

  AddCustomMetadataButton::AddCustomMetadataButton()
  {
    _button.set_icon_name("list-add-symbolic");
    _button.set_halign(Gtk::Align::END);
    _button.set_valign(Gtk::Align::CENTER);
    _button.set_hexpand(false);
    _button.set_has_frame(false);
    _button.add_css_class("ao-icon-button");
    _button.add_css_class("ao-detail-add-custom-metadata-button");
    _button.set_tooltip_text("Add Custom Metadata");
    _button.signal_clicked().connect([this] { openPopover(); });

    _popover.set_parent(_button);
    _popover.set_position(Gtk::PositionType::BOTTOM);
    _popover.set_autohide(true);
    _popover.set_has_arrow(true);
    _popover.add_css_class("ao-detail-custom-metadata-popover");

    _box.add_css_class("ao-detail-custom-metadata-popover-box");

    _titleLabel.set_halign(Gtk::Align::START);
    _titleLabel.add_css_class("ao-detail-custom-metadata-popover-title");
    _box.append(_titleLabel);

    _keyEntry.set_placeholder_text("Key");
    _keyEntry.set_hexpand(true);
    _keyEntry.add_css_class("ao-detail-custom-metadata-popover-entry");
    _valueEntry.set_placeholder_text("Value");
    _valueEntry.set_hexpand(true);
    _valueEntry.add_css_class("ao-detail-custom-metadata-popover-entry");

    _submitButton.set_label("Add");
    _submitButton.set_halign(Gtk::Align::END);
    _submitButton.set_hexpand(false);
    _submitButton.add_css_class("suggested-action");
    _submitButton.add_css_class("ao-detail-custom-metadata-popover-submit");
    _submitButton.set_tooltip_text("Add Custom Metadata");

    _submitButton.signal_clicked().connect([this] { handleAddRequested(); });
    _keyEntry.signal_activate().connect([this] { _valueEntry.grab_focus(); });
    _valueEntry.signal_activate().connect([this] { handleAddRequested(); });

    _keyEntry.property_text().signal_changed().connect([this] { _keyEntry.remove_css_class("error"); });
    _valueEntry.property_text().signal_changed().connect([this] { _valueEntry.remove_css_class("error"); });

    _box.append(_keyEntry);
    _box.append(_valueEntry);
    _actionBox.add_css_class("ao-detail-custom-metadata-popover-actions");
    _actionBox.set_halign(Gtk::Align::FILL);
    _actionBox.set_hexpand(true);
    _actionSpacer.set_hexpand(true);
    _actionBox.append(_actionSpacer);
    _actionBox.append(_submitButton);
    _box.append(_actionBox);
    _popover.set_child(_box);
  }

  AddCustomMetadataButton::~AddCustomMetadataButton()
  {
    _popover.popdown();
    _popover.unparent();
  }

  Gtk::Button& AddCustomMetadataButton::button()
  {
    return _button;
  }

  void AddCustomMetadataButton::markKeyError()
  {
    _keyEntry.add_css_class("error");
    _keyEntry.error_bell();
  }

  void AddCustomMetadataButton::markValueError()
  {
    _valueEntry.add_css_class("error");
    _valueEntry.error_bell();
  }

  void AddCustomMetadataButton::clearInputs()
  {
    _keyEntry.set_text("");
    _valueEntry.set_text("");
    _keyEntry.remove_css_class("error");
    _valueEntry.remove_css_class("error");
  }

  void AddCustomMetadataButton::popdown()
  {
    _popover.popdown();
  }

  sigc::signal<void(std::string, std::string)>& AddCustomMetadataButton::signalAddRequested()
  {
    return _addRequested;
  }

  void AddCustomMetadataButton::openPopover()
  {
    _popover.popup();
    _keyEntry.grab_focus();
  }

  void AddCustomMetadataButton::handleAddRequested()
  {
    auto const keyText = _keyEntry.get_text();
    auto const valueText = _valueEntry.get_text();

    auto key = trimmedCopy(keyText.raw());
    auto value = std::string{valueText.raw()};

    bool hasError = false;

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
