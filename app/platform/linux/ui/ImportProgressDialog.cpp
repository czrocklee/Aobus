// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/ui/ImportProgressDialog.h"

namespace app::ui
{

ImportProgressDialog::ImportProgressDialog(int maxItems, Gtk::Window& parent)
  : _maxItems{maxItems}
{
  set_title("Importing Music");
  set_transient_for(parent);
  set_modal(true);
  setupUi(maxItems);
}

ImportProgressDialog::~ImportProgressDialog() = default;

void ImportProgressDialog::setupUi([[maybe_unused]] int maxItems)
{
  constexpr int kDialogWidth = 400;
  constexpr int kDialogHeight = 150;
  constexpr int kBoxSpacing = 8;
  constexpr int kBoxMargin = 12;
  constexpr int kActionAreaSpacing = 6;

  set_default_size(kDialogWidth, kDialogHeight);

  auto box = Gtk::Box(Gtk::Orientation::VERTICAL, kBoxSpacing);
  box.set_margin(kBoxMargin);

  _progressLabel.set_text("Preparing to import...");
  _progressLabel.set_halign(Gtk::Align::START);
  box.append(_progressLabel);

  _progressBar.set_fraction(0.0);
  box.append(_progressBar);

  _okButton.set_label("OK");
  _okButton.set_sensitive(false);
  _okButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::OK); });
  box.append(_okButton);

  // Add action area for the button
  auto actionArea = Gtk::Box(Gtk::Orientation::HORIZONTAL, kActionAreaSpacing);
  actionArea.set_halign(Gtk::Align::END);
  actionArea.append(_okButton);
  actionArea.set_margin(kBoxMargin);
  box.append(actionArea);

  set_child(box);
}

void ImportProgressDialog::onNewTrack(std::string const& path, int itemIndex)
{
  auto fraction = static_cast<double>(itemIndex) / _maxItems;
  _progressBar.set_fraction(fraction);
  _progressLabel.set_text("Importing: " + path);
}

void ImportProgressDialog::ready()
{
  _progressBar.set_fraction(1.0);
  _progressLabel.set_text("Import complete!");
  _okButton.set_sensitive(true);
}

} // namespace app::ui
