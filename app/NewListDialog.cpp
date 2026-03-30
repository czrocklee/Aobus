// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "NewListDialog.h"

NewListDialog::NewListDialog(Gtk::Window& parent)
  : Gtk::Dialog()
{
  set_title("New List");
  set_transient_for(parent);
  set_modal(true);
  setupUi();
}

void NewListDialog::setupUi()
{
  set_default_size(400, 200);

  auto box = Gtk::Box(Gtk::Orientation::VERTICAL, 8);
  box.set_margin(12);

  // Name field
  auto nameLabel = Gtk::Label("Name:");
  nameLabel.set_halign(Gtk::Align::START);
  _nameEntry.set_placeholder_text("List name");
  box.append(nameLabel);
  box.append(_nameEntry);

  // Description field
  auto descLabel = Gtk::Label("Description:");
  descLabel.set_halign(Gtk::Align::START);
  _descEntry.set_placeholder_text("Optional description");
  box.append(descLabel);
  box.append(_descEntry);

  // Expression field
  auto exprLabel = Gtk::Label("Expression:");
  exprLabel.set_halign(Gtk::Align::START);
  _exprEntry.set_placeholder_text("Query expression (e.g., artist contains \"foo\")");
  box.append(exprLabel);
  box.append(_exprEntry);

  // Buttons
  auto buttonBox = Gtk::Box(Gtk::Orientation::HORIZONTAL, 6);
  buttonBox.set_halign(Gtk::Align::END);

  _cancelButton.set_label("Cancel");
  _cancelButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::CANCEL); });

  _okButton.set_label("Create");
  _okButton.set_sensitive(false);
  _okButton.signal_clicked().connect([this]() { response(Gtk::ResponseType::OK); });

  // Enable OK button when name is filled
  _nameEntry.signal_changed().connect([this]() { _okButton.set_sensitive(!_nameEntry.get_text().empty()); });

  buttonBox.append(_cancelButton);
  buttonBox.append(_okButton);
  buttonBox.set_margin(12);
  box.append(buttonBox);

  set_child(box);
}

app::model::ListDraft NewListDialog::draft() const
{
  app::model::ListDraft d;
  d.kind = app::model::ListKind::Smart;
  d.name = _nameEntry.get_text();
  d.description = _descEntry.get_text();
  d.expression = _exprEntry.get_text();
  // trackIds remain empty for smart lists
  return d;
}
