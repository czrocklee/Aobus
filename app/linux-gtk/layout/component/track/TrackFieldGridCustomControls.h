// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/LayoutConstants.h"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <sigc++/signal.h>

#include <string>

namespace ao::gtk::layout::track_field_grid
{
  class AddCustomMetadataButton final
  {
  public:
    AddCustomMetadataButton();
    ~AddCustomMetadataButton();

    AddCustomMetadataButton(AddCustomMetadataButton const&) = delete;
    AddCustomMetadataButton& operator=(AddCustomMetadataButton const&) = delete;
    AddCustomMetadataButton(AddCustomMetadataButton&&) = delete;
    AddCustomMetadataButton& operator=(AddCustomMetadataButton&&) = delete;

    Gtk::Button& button();

    void markKeyError();
    void markValueError();
    void clearInputs();
    void popdown();

    sigc::signal<void(std::string, std::string)>& signalAddRequested();

  private:
    void openPopover();
    void handleAddRequested();

    Gtk::Button _button;
    Gtk::Popover _popover;
    Gtk::Box _box{Gtk::Orientation::VERTICAL, kSpacingLarge};
    Gtk::Label _titleLabel{"Custom Metadata"};
    Gtk::Entry _keyEntry;
    Gtk::Entry _valueEntry;
    Gtk::Box _actionBox{Gtk::Orientation::HORIZONTAL, kSpacingMedium};
    Gtk::Label _actionSpacer;
    Gtk::Button _submitButton;

    sigc::signal<void(std::string, std::string)> _addRequested;
  };
} // namespace ao::gtk::layout::track_field_grid
