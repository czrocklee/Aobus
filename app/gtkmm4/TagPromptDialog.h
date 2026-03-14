#pragma once

#include <gtkmm.h>

#include <string>

class TagPromptDialog : public Gtk::Dialog
{
public:
  TagPromptDialog(Gtk::Window& parent);
  virtual ~TagPromptDialog() = default;

  std::string tag() const;

private:
  void setupUi();

  Gtk::Entry _tagEntry;
  Gtk::Button _okButton;
  Gtk::Button _cancelButton;
};
