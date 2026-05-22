// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "portal/LibraryTaskProgressDialog.h"

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
  namespace
  {
    constexpr int kDefaultDialogWidth = 400;
  }

  LibraryTaskProgressDialog::LibraryTaskProgressDialog(std::int32_t maxItems, Gtk::Window& parent)
    : Gtk::Dialog{"Library Task Progress", parent, true}
  {
    setupUi(maxItems);
  }

  LibraryTaskProgressDialog::~LibraryTaskProgressDialog() = default;

  void LibraryTaskProgressDialog::setupUi(std::int32_t /*maxItems*/)
  {
    set_default_size(kDefaultDialogWidth, -1);

    auto* const contentArea = get_content_area();
    auto* const box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, layout::kSpacingMedium);
    box->add_css_class("ao-dialog-content");

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

    contentArea->append(*box);

    add_button("OK", Gtk::ResponseType::OK);
    set_response_sensitive(Gtk::ResponseType::OK, false);
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
    set_response_sensitive(Gtk::ResponseType::OK, true);
  }
} // namespace ao::gtk::portal
