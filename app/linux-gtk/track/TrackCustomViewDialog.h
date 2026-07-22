// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/AppDialog.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/CustomPresentationEditorModel.h>

#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/listbox.h>
#include <gtkmm/window.h>

#include <optional>
#include <string_view>

namespace ao::gtk
{
  class TrackCustomViewDialog final : public AppDialog
  {
  public:
    TrackCustomViewDialog(Gtk::Window& parent,
                          rt::TrackPresentationSpec const& initialSpec,
                          std::string_view initialLabel);

    std::optional<rt::CustomTrackPresentationPreset> runDialog();

  private:
    void buildUi();
    void populateFromSpec(rt::TrackPresentationSpec const& spec, std::string_view label);
    rt::CustomTrackPresentationPreset collectState();

    void rebuildSortList();
    void rebuildVisibleFieldsList();

    Gtk::Entry _nameEntry;
    Gtk::DropDown _groupDropdown;
    Gtk::ListBox _sortTermsList;
    Gtk::ListBox _visibleFieldsList;

    uimodel::CustomPresentationEditorModel _model;
  };
} // namespace ao::gtk
