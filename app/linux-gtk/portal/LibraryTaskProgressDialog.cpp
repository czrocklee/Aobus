// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryTaskProgressDialog.h"

#include "app/AppDialog.h"
#include "layout/LayoutConstants.h"

#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/object.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <cstdint>
#include <string>

namespace ao::gtk::portal
{
  LibraryTaskProgressDialog::LibraryTaskProgressDialog(std::int32_t maxItems, Gtk::Window& parent)
    : AppDialog{}
  {
    set_title("Library Task Progress");
    set_transient_for(parent);
    setupUi(maxItems);
  }

  LibraryTaskProgressDialog::~LibraryTaskProgressDialog() = default;

  void LibraryTaskProgressDialog::setupUi(std::int32_t /*maxItems*/)
  {
    auto* const okButton = addPrimaryAction("OK", Gtk::ResponseType::OK);
    okButton->set_sensitive(false);
    // Store the button in a property if I need to access it later,
    // but AppDialog doesn't expose them. I'll add a member to LibraryTaskProgressDialog.
    _okButton = okButton;

    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingMedium);

    _titleLabel.set_markup("<b>Processing library...</b>");
    _titleLabel.set_halign(Gtk::Align::START);
    box->append(_titleLabel);

    _progressLabel.set_text("Starting...");
    _progressLabel.set_halign(Gtk::Align::START);
    _progressLabel.set_ellipsize(Pango::EllipsizeMode::END);
    box->append(_progressLabel);

    _progressBar.set_show_text(true);
    _progressBar.set_fraction(0.0);
    box->append(_progressBar);

    setContentWidget(*box);
  }

  void LibraryTaskProgressDialog::updateProgress(std::string const& message, double fraction)
  {
    _progressLabel.set_text(message);
    _progressBar.set_fraction(fraction);
  }

  void LibraryTaskProgressDialog::ready()
  {
    _titleLabel.set_markup("<b>Task complete</b>");
    _progressLabel.set_text("All items processed.");
    _progressBar.set_fraction(1.0);

    if (_okButton != nullptr)
    {
      _okButton->set_sensitive(true);
    }
  }
} // namespace ao::gtk::portal
