#include "TagPromptDialog.h"

#include <string>

TagPromptDialog::TagPromptDialog(Gtk::Window& parent) : Gtk::Dialog()
{
  set_title("Add Tag");
  set_transient_for(parent);
  set_modal(true);
  setupUi();
}

void TagPromptDialog::setupUi()
{
  set_default_size(300, 120);

  auto box = Gtk::Box(Gtk::Orientation::VERTICAL, 8);
  box.set_margin(12);

  // Tag input
  auto tagLabel = Gtk::Label("Tag:");
  tagLabel.set_halign(Gtk::Align::START);
  _tagEntry.set_placeholder_text("Enter tag name");
  box.append(tagLabel);
  box.append(_tagEntry);

  // Buttons
  auto buttonBox = Gtk::Box(Gtk::Orientation::HORIZONTAL, 6);
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
    if (!_tagEntry.get_text().empty())
    {
      response(Gtk::ResponseType::OK);
    }
  });

  buttonBox.append(_cancelButton);
  buttonBox.append(_okButton);
  buttonBox.set_margin(12);
  box.append(buttonBox);

  set_child(box);
}

std::string TagPromptDialog::tag() const { return _tagEntry.get_text(); }
