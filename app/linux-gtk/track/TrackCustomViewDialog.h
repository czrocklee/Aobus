// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/UIState.h"
#include <runtime/TrackPresentationPreset.h>

#include <gtkmm.h>
#include <optional>
#include <span>

namespace ao::gtk
{
  class TrackCustomViewDialog final : public Gtk::Dialog
  {
  public:
    struct Result final
    {
      CustomTrackPresentationState state;
      bool deleted = false;
    };

    TrackCustomViewDialog(Gtk::Window& parent,
                          rt::TrackPresentationSpec const& initialSpec,
                          std::string_view initialLabel);

    std::optional<Result> runDialog();

  private:
    void setupUi();
    void populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label);
    CustomTrackPresentationState collectState() const;

    void rebuildSortList();
    void rebuildVisibleFieldsList();

    Gtk::Entry _nameEntry;
    Gtk::DropDown _groupDropdown;
    Gtk::ListBox _sortTermsList;
    Gtk::ListBox _visibleFieldsList;

    Gtk::Button _saveButton{"Save"};

    std::vector<TrackPresentationSortTermState> _sortState;
    std::vector<std::uint8_t> _visibleFieldsState;
  };
} // namespace ao::gtk
