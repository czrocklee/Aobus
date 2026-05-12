// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ImportProgressDialog.h"

namespace ao::gtk
{
  ImportProgressDialog::ImportProgressDialog(std::int32_t maxItems, Gtk::Window& parent)
    : _maxItems{maxItems}
  {
    set_title("Importing Music");
    set_transient_for(parent);
    set_modal(true);
    setupUi(maxItems);
  }

  ImportProgressDialog::~ImportProgressDialog() = default;

  void ImportProgressDialog::setupUi(std::int32_t /*maxItems*/)
  {
    constexpr std::int32_t kDialogWidth = 400;
    constexpr std::int32_t kDialogHeight = 150;
    constexpr std::int32_t kBoxSpacing = 8;
    constexpr std::int32_t kBoxMargin = 12;
    constexpr std::int32_t kActionAreaSpacing = 6;

    set_default_size(kDialogWidth, kDialogHeight);

    auto box = Gtk::Box{Gtk::Orientation::VERTICAL, kBoxSpacing};
    box.set_margin(kBoxMargin);

    _progressLabel.set_text("Preparing to import...");
    _progressLabel.set_halign(Gtk::Align::START);
    box.append(_progressLabel);

    _progressBar.set_fraction(0.0);
    box.append(_progressBar);

    _okButton.set_label("OK");
    _okButton.set_sensitive(false);
    _okButton.signal_clicked().connect([this] { response(Gtk::ResponseType::OK); });
    box.append(_okButton);

    // Add action area for the button
    auto actionArea = Gtk::Box(Gtk::Orientation::HORIZONTAL, kActionAreaSpacing);
    actionArea.set_halign(Gtk::Align::END);
    actionArea.append(_okButton);
    actionArea.set_margin(kBoxMargin);
    box.append(actionArea);

    set_child(box);
  }

  void ImportProgressDialog::onNewTrack(std::string const& path, std::int32_t itemIndex)
  {
    auto const fraction = static_cast<double>(itemIndex) / _maxItems;
    _progressBar.set_fraction(fraction);
    _progressLabel.set_text("Importing: " + path);
  }

  void ImportProgressDialog::ready()
  {
    _progressBar.set_fraction(1.0);
    _progressLabel.set_text("Import complete!");
    _okButton.set_sensitive(true);
  }
} // namespace ao::gtk
