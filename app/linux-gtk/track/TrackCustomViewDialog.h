// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "runtime/TrackField.h"
#include "runtime/TrackPresentation.h"

#include <gtkmm/button.h>
#include <gtkmm/dialog.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/listbox.h>
#include <gtkmm/window.h>

#include <optional>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  class TrackCustomViewDialog final : public Gtk::Dialog
  {
  public:
    struct Result final
    {
      rt::CustomTrackPresentationPreset state;
      bool deleted = false;
    };

    TrackCustomViewDialog(Gtk::Window& parent,
                          rt::TrackPresentationSpec const& initialSpec,
                          std::string_view initialLabel);

    std::optional<Result> runDialog();

  private:
    void setupUi();
    void populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label);
    rt::CustomTrackPresentationPreset collectState() const;

    void rebuildSortList();
    void rebuildVisibleFieldsList();

    Gtk::Entry _nameEntry;
    Gtk::DropDown _groupDropdown;
    Gtk::ListBox _sortTermsList;
    Gtk::ListBox _visibleFieldsList;

    Gtk::Button _saveButton{"Save"};

    std::vector<rt::TrackSortTerm> _sortState;
    std::vector<rt::TrackField> _visibleFieldsState;

    std::vector<rt::TrackField> _availableVisibleFields;
    std::vector<rt::TrackSortField> _availableSortFields;
    std::vector<rt::TrackGroupKey> _availableGroupKeys;
  };
} // namespace ao::gtk
