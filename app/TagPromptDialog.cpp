// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "TagPromptDialog.h"

#include <string>

TagPromptDialog::TagPromptDialog(Gtk::Window& parent)
{
  set_title("Add Tag");
  set_transient_for(parent);
  set_modal(true);
  setupUi();
}

void TagPromptDialog::setupUi()
{
  constexpr int kDialogWidth = 300;
  constexpr int kDialogHeight = 120;
  constexpr int kBoxSpacing = 8;
  constexpr int kBoxMargin = 12;
  constexpr int kButtonBoxSpacing = 6;

  set_default_size(kDialogWidth, kDialogHeight);

  auto box = Gtk::Box(Gtk::Orientation::VERTICAL, kBoxSpacing);
  box.set_margin(kBoxMargin);

  // Tag input
  auto tagLabel = Gtk::Label("Tag:");
  tagLabel.set_halign(Gtk::Align::START);
  _tagEntry.set_placeholder_text("Enter tag name");
  box.append(tagLabel);
  box.append(_tagEntry);

  // Buttons
  auto buttonBox = Gtk::Box(Gtk::Orientation::HORIZONTAL, kButtonBoxSpacing);
  buttonBox.set_halign(Gtk::Align::END);

  _cancelButton.set_label("Cancel");
  _cancelButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::CANCEL); });

  _okButton.set_label("Add");
  _okButton.set_sensitive(false);
  _okButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::OK); });

  // Enable OK button when tag is filled
  _tagEntry.signal_changed().connect([this]() { _okButton.set_sensitive(!_tagEntry.get_text().empty()); });

  // Also activate on Enter key
  _tagEntry.signal_activate().connect([this]() {
    if (!_tagEntry.get_text().empty()) { response(Gtk::ResponseType::OK); }
  });

  buttonBox.append(_cancelButton);
  buttonBox.append(_okButton);
  buttonBox.set_margin(kBoxMargin);
  box.append(buttonBox);

  set_child(box);
}

std::string TagPromptDialog::tag() const
{
  return _tagEntry.get_text();
}
